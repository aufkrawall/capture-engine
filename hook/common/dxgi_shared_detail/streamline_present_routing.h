#pragma once

#include <dxgi1_4.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

// Forward declaration
class PerformanceMetrics;

// Post-SL overlay rendering callback.  Invoked during re-entrant Present
// (after Streamline's FG pipeline finishes) so the overlay renders AFTER FG
// interpolation - matching the standard inject-overlay approach for FG compatibility.
using PostSLOverlayRenderFn = void (*)(IDXGISwapChain* pSwapChain);
using PostSLStartupActivationServiceFn = bool (*)(const char* source, bool clearStartupWindow);

#include "types_and_state.h"

// Streamline present classification and Post-SL startup handoff routing.

namespace DXGIShared {

inline bool ShouldTreatStreamlinePresentAsSyntheticReentrant(
    bool isD3D12SwapChain, bool streamlineFGRunning, bool callerFromStreamlineModule, bool postSLConfirmedRendering,
    bool postSLConfirmedButStartupSettling, bool streamlineStartupHandoffInProgress, bool presentOwnershipActive,
    bool recentLargePresentGap, bool matchesExpectedPresentThread, bool startupTopLevelPresentAlreadyConsumed) {
    if (!(isD3D12SwapChain && streamlineFGRunning && callerFromStreamlineModule)) {
        return false;
    }

    // Once PostSL has already confirmed a successful render and the explicit
    // confirmed-startup-settling window has ended, a later Streamline-originated
    // Present that arrives with no active Present owner is no longer just a
    // synthetic worker-thread recursion candidate. That is the live FG Present
    // path resurfacing as a standalone top-level call, and forcing it through the
    // synthetic/bypass path corrupts the active DX12 FG chain.
    //
    // During the short confirmed-startup-settling window, however, GTA still
    // needs these standalone Streamline Presents to classify as synthetic first
    // so the later callback-on-normal-route split can keep PostSL rendering
    // advancing without sending the Present itself down the old bypass path.
    if (postSLConfirmedRendering && !postSLConfirmedButStartupSettling && !presentOwnershipActive) {
        return false;
    }

    // Late DLSS runtime-owned handoffs can surface the first live Present only
    // after the game already stopped issuing top-level Presents for a few
    // hundred milliseconds. Treating that first startup-window Present as a
    // synthetic bypass starves the normal Present path and PostSL never
    // bootstraps. Keep the synthetic route for true recursive/worker-thread
    // cases, but let large-gap startup-handoff Presents run as top-level
    // Presents when no other Present currently owns the path, the call is still
    // arriving on the expected game-present thread, and we have not already
    // consumed that one-time startup bootstrap Present for the current handoff.
    if (streamlineStartupHandoffInProgress && recentLargePresentGap && !presentOwnershipActive &&
        matchesExpectedPresentThread && !startupTopLevelPresentAlreadyConsumed) {
        return false;
    }

    return true;
}

inline bool ShouldUseStreamlineStartupTopLevelCandidate(bool observerOnlyMode, bool streamlineSyntheticReentrant,
                                                        bool callerFromStreamlineModule, bool isD3D12SwapChain,
                                                        bool streamlineFGRunning,
                                                        bool streamlineStartupHandoffInProgress,
                                                        bool recentLargePresentGap, bool matchesExpectedPresentThread,
                                                        bool postSLConfirmedRendering) {
    // The large-gap promotion is a one-shot bootstrap route. Once PostSL has
    // already submitted successfully, reclassifying later standalone output
    // Presents as the original handoff bypass suppresses the proven PostSL
    // callback until the large-gap window expires and visibly drains the
    // overlay from every swapchain buffer.
    return !observerOnlyMode && !streamlineSyntheticReentrant && callerFromStreamlineModule && isD3D12SwapChain &&
           streamlineFGRunning && streamlineStartupHandoffInProgress && recentLargePresentGap &&
           matchesExpectedPresentThread && !postSLConfirmedRendering;
}

inline bool ShouldRenderExactPostSLBeforeStartupHandoffTransport(bool isD3D12SwapChain, bool hadFSRFGPhase,
                                                                 bool safePostFSRBootstrapPath,
                                                                 bool streamlineFGRunning,
                                                                 bool startupTopLevelCandidate,
                                                                 bool postSLConfirmedRendering) {
    // The first runtime-owned Present after FSR can precede the ordinary PostSL
    // callback by one output. Once the post-FSR queue topology is proven safe,
    // draw onto that exact proxy backbuffer before forwarding it. Cold DLSS and
    // already-confirmed routes keep their established paths unchanged.
    return isD3D12SwapChain && hadFSRFGPhase && safePostFSRBootstrapPath && streamlineFGRunning &&
           startupTopLevelCandidate && !postSLConfirmedRendering;
}

inline bool ShouldAllowSpecialStreamlinePresentRouting(bool observerOnlyMode) {
    // The narrowed observer-startup-present-only seam now preserves only the
    // non-Streamline startup-Present probe pieces such as the FFX startup
    // bypass. Special Streamline-originated Present routing stays passive in
    // all observer modes so the staged seam can be compared cleanly against the
    // stable observer-policy-only baseline.
    return !observerOnlyMode;
}

inline bool ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
    bool observerOnlyMode, bool hadFSRFGPhase, bool explicitSetOptionsActivation, bool safePostFSRBootstrapPath,
    bool startupTopLevelPresentConsumed, bool callerFromStreamlineModule, bool postSLStartupActivationPending,
    bool postSLActiveButUnconfirmed, bool postSLConfirmedButStartupSettling, bool streamlineSyntheticReentrant) {
    // Once the pure-DLSS startup family has already consumed its one-shot
    // top-level bootstrap, the next decisive Streamline-originated Present can be
    // the only callback opportunity before the runtime either settles or stalls.
    // Sending that call down the old synthetic/bypass path can strand PostSL both
    // before activation and during the first post-activation warm-up callbacks
    // before PostSL has ever confirmed a successful render. GTA's latest active
    // validation also showed one more seam: the very next Streamline-originated
    // Present immediately after the first successful PostSL submits can still be
    // part of the same fragile startup family. Keep the call on the normal SL
    // route instead until startup is no longer half-armed and those first
    // confirmed PostSL frames have settled.
    // The post-FSR comeback family is stricter than cold pure-DLSS startup, but
    // it also already has stronger ownership evidence: the fresh runtime-owned
    // Streamline handoff queue preserved across the FSR->DLSS transition. Once
    // that stricter family is still half-armed, forcing its first comeback
    // Presents down the old synthetic/bypass return path reopens the old crash
    // seam before the post-FSR bootstrap can progress. Talos showed two more
    // nuances: a GetState-only comeback is still weaker evidence than an explicit
    // SetOptions activation edge, but some runtimes can also reach a safe
    // post-FSR bootstrap topology before any OFF->ON SetOptions edge ever
    // appears. Once that safe topology is already proven, continuing to force the
    // comeback through synthetic/bypass just starves PostSL until the same old NX
    // crash family returns later.
    const bool startupHalfArmed =
        postSLStartupActivationPending || postSLActiveButUnconfirmed || postSLConfirmedButStartupSettling;
    return !observerOnlyMode && callerFromStreamlineModule && startupHalfArmed && streamlineSyntheticReentrant &&
           (startupTopLevelPresentConsumed ||
            (hadFSRFGPhase && (explicitSetOptionsActivation || safePostFSRBootstrapPath)));
}

inline bool ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(bool isD3D12SwapChain, bool hadFSRFGPhase,
                                                                            bool startupTopLevelCandidate,
                                                                            bool safePostFSRBootstrapPath,
                                                                            bool staleThirdPartyPresentHookRisk) {
    // The first large-gap Streamline startup-handoff Present can still be the
    // only top-level call that re-establishes the live route after an FSR-owned
    // swapchain handoff. That Present should remain logically on the normal SL
    // route so startup-policy state can advance, but the actual transport is
    // still fragile while the recovered queue topology has only just been
    // proven. A stale third-party Present hook is one risk; the FSR->DLSS
    // runtime handoff itself is another even when no external overlay is
    // present. Keep the route, but transport through the bypass trampoline.
    return isD3D12SwapChain && hadFSRFGPhase && startupTopLevelCandidate &&
           (safePostFSRBootstrapPath || staleThirdPartyPresentHookRisk);
}

inline bool ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
    bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain, bool bypassAvailable,
    bool callerFromStreamlineModule, bool streamlineStartupHandoffInProgress, bool runtimeOwnedSwapchainActive,
    bool startupNormalRouteCandidate, bool postFSRRuntimeHandoffRisk, bool thirdPartyPresentHookRisk) {
    // Runtime ownership alone is not a transport hazard. GTA's all-off -> DLSS FG
    // Apply path needs real Streamline Present traffic after the EOS-backed
    // runtime-owned handoff, otherwise DLSSG startup never activates. Use bypass
    // transport only when the same startup family also carries a stale/fragile
    // third-party Present hook risk or a proven post-FSR runtime handoff risk.
    return isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain && bypassAvailable &&
           callerFromStreamlineModule && streamlineStartupHandoffInProgress && runtimeOwnedSwapchainActive &&
           startupNormalRouteCandidate && (postFSRRuntimeHandoffRisk || thirdPartyPresentHookRisk);
}

inline bool ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(bool isD3D12SwapChain,
                                                                               bool startupTopLevelCandidate,
                                                                               bool startupNormalRouteTransportRisk,
                                                                               bool staleThirdPartyPresentHookRisk) {
    return isD3D12SwapChain && startupTopLevelCandidate &&
           (startupNormalRouteTransportRisk || staleThirdPartyPresentHookRisk);
}

inline bool ShouldActivateStreamlinePresentRoutingForHookTarget(bool entryHookDetected, bool hookTargetResolved,
                                                                bool hookTargetFromStreamlineModule,
                                                                bool hookTargetFromCaptureHookModule) {
    // dxgi!Present can begin with a jump for several reasons: CE's own inline
    // hook, DXGI's internal thunking, Steam/EOS hook chains, or Streamline. Only
    // the Streamline target is safe to route through as the SL FG chain.
    return entryHookDetected && hookTargetResolved && hookTargetFromStreamlineModule &&
           !hookTargetFromCaptureHookModule;
}

inline bool ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
    bool observerOnlyMode, bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain,
    bool originalPresentAvailable, bool streamlineFGRunning, bool streamlinePresentRoutingActive,
    bool callerFromStreamlineModule, bool streamlineStartupHandoffInProgress, bool runtimeOwnedSwapchainActive,
    bool hadFSRFGPhase, bool safePostFSRBootstrapPath, bool postSLConfirmedRendering,
    bool startupTopLevelPresentAlreadyConsumed) {
    // Some real runtimes surface the first post-FSR Streamline startup handoff as
    // an app-thread Present on the freshly runtime-owned swapchain before any
    // Streamline-originated top-level Present reaches us. Treat that first call
    // as the startup bootstrap transport hazard too; otherwise the normal SL
    // route can enter the driver after CE has injected overlay work, while a
    // direct DXGI bypass skips Streamline's mandatory startup handling. Keep the
    // first handoff overlayless but route it through Streamline.
    return !observerOnlyMode && isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain &&
           originalPresentAvailable && streamlineFGRunning && streamlinePresentRoutingActive &&
           !callerFromStreamlineModule && streamlineStartupHandoffInProgress && runtimeOwnedSwapchainActive &&
           hadFSRFGPhase && safePostFSRBootstrapPath && !postSLConfirmedRendering &&
           !startupTopLevelPresentAlreadyConsumed;
}

inline bool ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
    bool observerOnlyMode, bool hadFSRFGPhase, bool explicitSetOptionsActivation,
    bool activeDLSSFGRuntimeSignalObserved, bool safePostFSRBootstrapPath, bool postSLStartupActivationPending,
    bool postSLActiveButUnconfirmed, bool postSLStartupActivationEntered, bool postSLConfirmedButStartupSettling,
    bool streamlineSyntheticReentrant) {
    // Once PostSL has already confirmed at least one successful render, GTA's
    // startup family can still need a few more Streamline-originated Presents to
    // advance the stable-frame counter and keep the visible overlay alive. Those
    // calls should still execute the PostSL callback, but they must stay on the
    // normal SL route instead of taking the old synthetic/bypass return path.
    if (observerOnlyMode || !streamlineSyntheticReentrant) {
        return false;
    }

    if (postSLConfirmedButStartupSettling) {
        return true;
    }

    // Once the retained-startup service has entered PostSL, progress must move
    // through the normal Streamline Present family. Blocking the callback here
    // leaves pure-DLSS resume paths active-but-unconfirmed forever.
    if (postSLStartupActivationEntered && postSLActiveButUnconfirmed) {
        return true;
    }

    // Pure-DLSS startup is allowed to enter PostSL once the app has either
    // explicitly requested DLSS-G or the official DLSS-G state API reports an
    // active runtime signal. Both are stronger than "Streamline is present" and
    // keep the overlay from waiting for the whole startup timer before PostSL can
    // even begin its own protected warmup.
    if (!hadFSRFGPhase && (explicitSetOptionsActivation || activeDLSSFGRuntimeSignalObserved) &&
        postSLStartupActivationPending) {
        return true;
    }

    // Post-FSR comeback still intentionally uses the older repeated-callback
    // stabilization path before PostSL fully confirms rendering, but only once
    // the bootstrap topology itself is already safe.  We no longer require an
    // explicit SetOptions(ON) edge here: the safe bootstrap path already proves
    // the queue topology can handle the callback, and requiring both conditions
    // can strand PostSL indefinitely when SetOptions has not yet been observed
    // but the handoff queue is already live (FSR->OFF->DLSS transitions).
    return hadFSRFGPhase && safePostFSRBootstrapPath && (postSLStartupActivationPending || postSLActiveButUnconfirmed);
}

inline bool ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(
    bool isD3D12SwapChain, bool keepStartupPresentOnNormalRoute, bool postSLConfirmedRendering,
    bool postSLConfirmedButStartupSettling, bool startupNormalRouteTransportRisk, bool staleThirdPartyPresentHookRisk) {
    // After a fresh runtime-owned Streamline handoff, the shared routing layer can
    // correctly decide that decisive startup Presents must stay in the normal
    // Streamline family so PostSL keeps making progress. If that same family has
    // a stale third-party hook chain, keep the callback/routing decision but
    // transport the actual Present through the bypass trampoline until PostSL has
    // both confirmed a successful render and left startup settling.
    return isD3D12SwapChain && keepStartupPresentOnNormalRoute &&
           (!postSLConfirmedRendering || postSLConfirmedButStartupSettling) &&
           (startupNormalRouteTransportRisk || staleThirdPartyPresentHookRisk);
}

inline bool ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
    bool observerOnlyMode, bool isD3D12SwapChain, bool streamlineFGRunning, bool callerFromStreamlineModule,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling, bool presentOwnershipActive,
    bool streamlineSyntheticReentrant) {
    // Some DLSS FG runtimes surface the steady-state generated-frame Present as a
    // standalone top-level Streamline call with no active Present owner instead
    // of a later recursive callback. Keep that Present on the normal SL route,
    // but still invoke PostSL there once the explicit startup-settling window has
    // ended so visible PostSL rendering does not starve after the first few
    // confirmed startup frames.
    return !observerOnlyMode && isD3D12SwapChain && streamlineFGRunning && callerFromStreamlineModule &&
           postSLConfirmedRendering && !postSLConfirmedButStartupSettling && !presentOwnershipActive &&
           !streamlineSyntheticReentrant;
}

inline bool ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
    bool isD3D12SwapChain, bool hadFSRFGPhase, bool invokePostSLOnConfirmedStandaloneNormalRoute,
    bool staleThirdPartyPresentHookRisk) {
    // Talos still reaches the stale-Steam-hook crash family after the post-FSR
    // comeback has already left the earlier startup-bypass window. At that later
    // boundary the Present is no longer classified as synthetic startup traffic;
    // it is the confirmed standalone Streamline Present that keeps PostSL alive
    // on the recovered swapchain. Routing should stay on the normal Streamline
    // family, but DX12 transport still needs the bypass trampoline on post-FSR
    // comebacks so we do not fall back through a fresh-swapchain third-party hook
    // chain whose saved original Present pointer is still stale.
    return isD3D12SwapChain && hadFSRFGPhase && invokePostSLOnConfirmedStandaloneNormalRoute &&
           staleThirdPartyPresentHookRisk;
}

inline bool ShouldBypassFFXPresentDuringStreamlineStartup(bool isD3D12SwapChain, bool callerFromFFXFGModule,
                                                          bool streamlineStartupHandoffPending,
                                                          bool streamlineStartupTransitionWindowActive,
                                                          bool observerOnlyMode, bool observerStartupPresentOnlyMode) {
    // During repeated FSR->DLSS handoffs, FFX teardown Presents can arrive
    // before Streamline publishes its running signal or after an older PostSL
    // path clears the pending latch for the new epoch. A short explicit
    // transition window keeps those Presents on the safe bypass route instead
    // of falling through the generic oPresent path into third-party hook
    // chains.
    return !(observerOnlyMode && !observerStartupPresentOnlyMode) && isD3D12SwapChain && callerFromFFXFGModule &&
           (streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive);
}

inline bool ShouldKeepPostSLCallbackInstalledDuringTransition(bool streamlineFGRunningAfterTransition) {
    return streamlineFGRunningAfterTransition;
}

// Vulkan-layer label selection for translated APIs should prefer the active DXVK
// D3D11 path over a merely-present DXVK D3D9 helper DLL in the same folder.
inline const char* SelectTranslatedGraphicsAPIName(bool hasDxvkD3D11, bool hasDxvkD3D9, bool hasVkd3dD3D12,
                                                   bool hasDX10) {
    if (hasDxvkD3D11) {
        return hasDX10 ? "DX10 (DXVK)" : "DX11 (DXVK)";
    }
    if (hasDxvkD3D9) {
        return "DX9 (DXVK)";
    }
    if (hasVkd3dD3D12) {
        return "DX12 (VKD3D-Proton)";
    }
    return "Vulkan";
}

// Direct-call helpers: bypass vtable hooks by calling saved original function
// pointers directly. Used by CWrapDXGISwapChain to avoid re-entry through
// hooked vtable.
HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pParams);

// DX12 marks known foreign overlay swapchains so Present can bypass full
// ProcessFrame/queue-tracking on those auxiliary chains.
void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath = nullptr);
void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain);

}  // namespace DXGIShared
