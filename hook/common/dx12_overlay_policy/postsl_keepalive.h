#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

#include "postsl_queue_selection.h"

// Pure-DLSS cold start and confirmed Post-SL keep-alive across Streamline off/on edges.

namespace ce::dx12_overlay_policy {

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

inline bool ShouldDriveExactPostSLOffKeepAliveBeforePresent(bool keepAliveLatched, bool streamlineFGRunning,
                                                            bool fsrFGApiActive, bool runtimeOwnedNativeFGPresentPath,
                                                            bool protectedOfficialFFXStartup,
                                                            bool streamlineModulesLoaded, bool callbackExecutionEnabled,
                                                            bool callbackInstalled, bool hasPostSLRenderQueue,
                                                            bool currentSwapchainMatchesLastSuccessfulPostSLSwapchain) {
    // A runtime proxy may keep issuing Present after DLSS-G is explicitly OFF
    // while bypassing every ordinary ProcessFrame entry (for example, through a
    // wrapped/pass-through Present). Drive the already-proven PostSL route before
    // any such early return. Exact swapchain + successful queue proof are
    // mandatory, and native FSR ownership always wins the GPU-quiesce boundary.
    return keepAliveLatched && !streamlineFGRunning && !fsrFGApiActive && !runtimeOwnedNativeFGPresentPath &&
           !protectedOfficialFFXStartup && streamlineModulesLoaded && callbackExecutionEnabled && callbackInstalled &&
           hasPostSLRenderQueue && currentSwapchainMatchesLastSuccessfulPostSLSwapchain;
}

inline bool ShouldSubmitInactiveDLSSExactPostSLKeepAlive(bool preRoutingKeepAliveAlreadySubmitted) {
    // The top-level Present pre-routing hook covers wrapped/pass-through routes
    // before any early return. ProcessFrame reaches the same exact-proxy route on
    // the ordinary path; submitting again would blend two independently sampled
    // overlay snapshots into one backbuffer and retain stale content across
    // buffer reuse. ProcessFrame is only the fallback when pre-routing failed.
    return !preRoutingKeepAliveAlreadySubmitted;
}

inline bool ShouldPreserveConfirmedPostSLProxyResourcesAcrossOuterOff(bool streamlineTurnedOff,
                                                                      bool postSLExplicitOffKeepAlive,
                                                                      bool currentSwapchainMatchesLastSuccessfulPostSL,
                                                                      bool hasOverlayBackend, bool hasSyncBackend,
                                                                      bool deviceRemoved) {
    // The keep-alive is make-before-break only if the outer ProcessFrame path
    // leaves the exact, already-proven proxy backend intact. Rebuilding the same
    // RTV/fence state after first destroying it creates a one-present blank and
    // an unnecessary transition-time drain. Unknown/replaced swapchains and a
    // removed device retain the strict teardown path.
    return streamlineTurnedOff && postSLExplicitOffKeepAlive && currentSwapchainMatchesLastSuccessfulPostSL &&
           hasOverlayBackend && hasSyncBackend && !deviceRemoved;
}

inline bool ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
    bool keepAliveRenderAfterExplicitOff, bool currentSwapchainMatchesLastSuccessfulPostSLSwapchain,
    bool hasPostSLLastWorkingQueue) {
    // Explicit OFF clears the transient swapchain-queue capture and can leave an
    // older epoch lock behind. The retained last-working queue is the exact
    // successful direct submit path which armed this swapchain keep-alive.
    return keepAliveRenderAfterExplicitOff && currentSwapchainMatchesLastSuccessfulPostSLSwapchain &&
           hasPostSLLastWorkingQueue;
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

// Startup-transport bypass / overlayless handoff retains (the "startup normal-route bypass",
// "startup-handoff normal-route bypass", and "app-thread post-FSR startup-handoff overlayless SL route"
// present paths). Retention exists ONLY for PostSL STARTUP recovery: the single release fires at "PostSL
// confirmed rendering", and the retained slot is never even consulted after confirmation
// (ShouldPreferRetainedStreamlineStartupActivationSwapchain requires pending/unconfirmed). A retain AFTER
// confirmation is therefore a pure COM-reference LEAK that pins the swapchain — and with it the HWND
// association DXGI checks on the next CreateSwapChainForHwnd. GTA FSR->DLSS apply (session
// 20260702_092933): bypass retains generations 3-11 landed after confirmation, CE's leaked reference kept
// AMD's old FI real swapchain alive, the game's replacement swapchain create failed E_ACCESSDENIED, and
// GTA null-dereferenced the missing swapchain. These transport paths must stop retaining at confirmation.
inline bool ShouldRetainStreamlineStartupActivationSwapchainFromStartupTransport(bool isD3D12SwapChain,
                                                                                 bool postSLConfirmedRendering) {
    return isD3D12SwapChain && !postSLConfirmedRendering;
}

// How the CreateSwapChainForHwnd E_ACCESSDENIED recovery runs. E_ACCESSDENIED means the HWND still has a
// live swapchain — possibly pinned by CE's OWN references (the retained startup-activation swapchain).
// For CE/game-owned creates the full overlay cleanup + retry is correct. For runtime-managed creates
// (Streamline/FFX handoff, third-party overlay in the call chain) CE used to pass the error through
// untouched ("don't disturb the runtime's handoff state machine") — but a failed runtime create is FATAL
// to the game (GTA dereferences the null swapchain and crashes, session 20260702_092933), so blind
// pass-through is never acceptable: first do the MINIMAL CE-owned unpin (release the retained
// startup-activation swapchain, no overlay teardown / GPU flush) and retry; escalate to the full cleanup
// only if the HWND stays pinned (a disturbed handoff beats a guaranteed crash).
enum class CreateSwapchainAccessDeniedRecovery {
    kFullOverlayCleanupAndRetry,    // CE/game-owned create: existing full cleanup + retry
    kMinimalCEReleaseThenEscalate,  // runtime-managed create: minimal CE unpin + retry, escalate if still denied
};

inline CreateSwapchainAccessDeniedRecovery ChooseCreateSwapchainAccessDeniedRecovery(
    bool passThroughForRuntimeManagedFG, bool callerFromThirdPartyOverlay) {
    return (passThroughForRuntimeManagedFG || callerFromThirdPartyOverlay)
               ? CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate
               : CreateSwapchainAccessDeniedRecovery::kFullOverlayCleanupAndRetry;
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

inline bool ShouldLetSyntheticPostSLProgressDuringOverlayReinitCooldown(bool streamlineFGRunning,
                                                                        bool startupActivationPending,
                                                                        bool postSLActiveButUnconfirmed,
                                                                        bool postSLConfirmedRendering,
                                                                        bool postSLConfirmedButStartupSettling) {
    // The FG transition cooldown still protects the unsafe pre-SL/reinit path,
    // but a half-armed or freshly confirmed PostSL startup route is already on
    // Streamline's own Present callback timing. Re-applying the generic cooldown
    // to PostSL itself blanks the overlay during DLSS resume even though the only
    // remaining work is rebuilding resources on the new authoritative SL
    // swapchain from inside PostSL.
    return streamlineFGRunning && ShouldKeepSyntheticStartupStateUntilConfirmedRender(
                                      startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
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
    bool swapchainQueueDiffersFromOriginalGameQueue, bool fsrFGApiActive, bool runtimeOwnedNativeFGPresentPath,
    bool currentSwapchainMatchesLastSuccessfulPostSL, bool hasConfirmedPostSLRenderQueue,
    bool warmResumePreservationPending) {
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
    //
    // A suspend -> warm-resume can legitimately clear transient swapchain-queue
    // bookkeeping before the first synthetic wrapper Present. In that topology,
    // either exact equality with the last successfully submitted PostSL
    // swapchain, or the event-driven warm-resume marker before the first active
    // submit, plus the retained render queue is stronger evidence than runtime-
    // ownership bookkeeping and remains valid after the historical 30-frame
    // warmup range. A real FSR/native takeover is still an absolute veto.
    (void)hadFSRFGPhase;
    if (!streamlineFGRunning || !postSLConfirmedRendering || fsrFGApiActive || runtimeOwnedNativeFGPresentPath) {
        return false;
    }
    const bool runtimeQueueProof = confirmedPostSLBackendWarmupProtected && runtimeOwnsSwapchain && hasSwapchainQueue &&
                                   hasOriginalGameQueue && swapchainQueueDiffersFromOriginalGameQueue;
    const bool retainedSuccessfulRouteProof =
        hasConfirmedPostSLRenderQueue && (currentSwapchainMatchesLastSuccessfulPostSL || warmResumePreservationPending);
    return runtimeQueueProof || retainedSuccessfulRouteProof;
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

}  // namespace ce::dx12_overlay_policy
