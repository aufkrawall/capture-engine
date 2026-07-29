#pragma once

#include <windows.h>

#include <cstdint>

#include <dxgi1_6.h>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"
#include "../overlay_fg_metric_policy.h"

struct ID3D12CommandQueue;
struct ID3D12Fence;

#include "ffx_routing.h"

// FG toggle transitions, overlay metrics binding, and published FG status types.

namespace ce::dx12_overlay_policy {

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
    return static_cast<int>(overlay_metrics::ResolveFGMetricType(effectiveFGActive, effectiveRuntimeMode));
}

inline bool DoOverlayFGPublishedTypesDiffer(bool lhsFGActive, fg_runtime::RuntimeMode lhsRuntimeMode, bool rhsFGActive,
                                            fg_runtime::RuntimeMode rhsRuntimeMode) {
    return overlay_metrics::DoPublishedFGTypesDiffer(lhsFGActive, lhsRuntimeMode, rhsFGActive, rhsRuntimeMode);
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
                                                                       bool swapchainQueueMatchesOriginalGameQueue,
                                                                       bool currentSwapchainMatchesCapturedQueue) {
    // A fresh swapchain recreation captured on the original Present queue is the
    // strongest non-heuristic signal we have that ownership has returned to the
    // normal non-FG topology. End the post-FSR recovery immediately in that case
    // instead of waiting for later cleanup paths to notice indirectly.
    return endingPostFSRNonFGRecovery && hasSwapchainQueue && hasOriginalGameQueue &&
           swapchainQueueMatchesOriginalGameQueue && currentSwapchainMatchesCapturedQueue;
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
                                                    bool postFSRNonFGRecovery, ce::fg_runtime::RuntimeMode runtimeMode,
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
    bool authoritativeFSRActive, bool runtimeOwnedNativeFGPresentPath, bool ffxPresentCallbackFallbackAllowed = false,
    bool nativeFSRInternalNoCallbackComposition = false, bool ffxUiResourceCompositionActive = false,
    bool liveSwapchainQueueIsOriginalGameQueue = false, bool fsrFGDisabledSuspendPending = false) {
    if (streamlineFGRunning) {
        return false;
    }

    const bool nativeFSRPresentOwnership =
        authoritativeFSRActive || fg_runtime::RuntimeModeUsesFSR(runtimeMode) || runtimeOwnedNativeFGPresentPath;
    if (!nativeFSRPresentOwnership) {
        return false;
    }

    // No-app-callback native FSR: AMD keeps its internal composition. The overlay reaches FG frames by CE
    // drawing onto the game's REGISTERED UI resource (AMD blends it post-interpolation on its own queue) —
    // see DX12_CompositeOverlayOntoFFXUiResource. While that route is live we MUST skip the separate overlay
    // submit on AMD's runtime present queue: that foreign ECL on AMD's pacing-critical queue is exactly what
    // wedges AMD's presenter (ffxQuery, sessions 20260618_155443 / 20260618_201038). If the game does NOT
    // register a UI resource (composition not active), fall back to the legacy runtime-queue route so the
    // overlay is at least visible.
    if (nativeFSRInternalNoCallbackComposition) {
        // Must AGREE with ChooseNoCallbackFSRFGOverlayRoute (single source of truth): while AMD owns the
        // swapchain the overlay rides the UI-resource composition BUNDLE and CE submits ZERO backbuffer /
        // separate overlay GPU work on AMD's runtime queue — that submit CRASHES during active interpolation
        // and STALLS the app to ~1 fps during a suspension (its GPU-completion fence never signals because
        // AMD stops flushing its runtime queue while suspended; session 20260703_210021). So `skip` here is
        // NOT gated on the suspend flag — suspend still skips the separate work, exactly like active FG, and
        // the bundle composite (route kSkipBundleCovers) keeps the overlay drawing on CE's own fenced queue.
        // The ONLY case that does NOT skip is a STALE latch after the game recreated its OWN native swapchain
        // (live queue back on origGame): AMD's FI swapchain is gone, the bundle is invisible, and the
        // backbuffer submit on the game's own queue is safe + necessary. ffxUiResourceCompositionActive is the
        // cached-UI-texture latch (DX12_IsFFXUiResourceCachedForBundle) — no cache means no bundle, so fall
        // through to the normal route.
        const bool bundleCovers = ffxUiResourceCompositionActive && !liveSwapchainQueueIsOriginalGameQueue;
        (void)runtimeOwnsSwapchain;
        (void)ffxPresentCallbackFallbackAllowed;
        (void)fsrFGDisabledSuspendPending;
        return bundleCovers;
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
    bool currentFFXPresentCallbackProof, bool progressResolvedStableOverlayProof = false, ULONGLONG stallDurationMs = 0,
    bool explicitNativeFSROffPendingRuntimeOwnedTeardown = false) {
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
inline bool ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(bool recognizedFrameGenerationConfigure,
                                                                             bool frameGenerationEnabled,
                                                                             bool appPresentCallbackProvided,
                                                                             bool hasExistingBridgeWithOriginal) {
    return recognizedFrameGenerationConfigure && frameGenerationEnabled && !appPresentCallbackProvided &&
           hasExistingBridgeWithOriginal;
}

inline bool ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(bool runtimeOwnedNativeFGPresentPath,
                                                                     bool nativeFSRActive,
                                                                     bool ffxPresentCallbackStalled,
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
    // Only WRAP a REAL app/default present callback. Synthesizing CE's bridge where the game provided
    // NO callback was tried twice (1b71d43, then 8acb8fd) and BOTH times wedged AMD's ffxQuery in ~8
    // frames: installing a callback flips AMD's FfxFrameInterpolationSwapchain out of its native internal
    // no-callback composition mode, and CE's synthesized current->output compose (GPU-breadcrumb-proven to
    // COMPLETE on the GPU, session 20260618_155443) still leaves AMD's presenter parked forever on its
    // auto-reset pacing event (handle 0x35EC, Waiting). The breadcrumb FALSIFIED 8acb8fd's "exact states
    // fix it" hypothesis. For the no-app-callback case CE must instead PRESERVE AMD's internal composition
    // (see ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration: nativeFSRInternalNoCallbackComposition
    // keeps the normal DX12 overlay route). Disabled / startup-arming packets stay excluded by
    // `frameGenerationEnabled` (documented GTA fail-fast on a synthetic callback before the enabled configure).
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

inline bool ShouldBridgeOverlayViaFFXPresentCallback(bool runtimeOwnedNativeFGPresentPath, bool authoritativeFSRActive,
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
    (void)dxgiFormat;
    return false;
}

inline bool IsPresentationContractDependentFormat(int dxgiFormat) {
    return dxgiFormat == static_cast<int>(DXGI_FORMAT_R10G10B10A2_UNORM) ||
           dxgiFormat == static_cast<int>(DXGI_FORMAT_R10G10B10A2_TYPELESS) ||
           dxgiFormat == static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT) ||
           dxgiFormat == static_cast<int>(DXGI_FORMAT_R16G16B16A16_TYPELESS);
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

inline bool ResolveActualHDRStateForOverlayTarget(int dxgiFormat, bool hasSwapChainColorSpace, int colorSpace) {
    return ce::presentation_color::IsHDR(ce::presentation_color::ResolveDXGI(
        static_cast<DXGI_FORMAT>(dxgiFormat), hasSwapChainColorSpace,
        static_cast<DXGI_COLOR_SPACE_TYPE>(colorSpace)));
}

inline bool ResolveRuntimeOwnedCallbackHDRStateFromCachedState(int dxgiFormat, bool hasCachedHDRState,
                                                               bool cachedHDRState) {
    if (!IsPresentationContractDependentFormat(dxgiFormat)) {
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

}  // namespace ce::dx12_overlay_policy
