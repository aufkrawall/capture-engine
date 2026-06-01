#pragma once

#include <cstddef>
#include <cstdint>

#include "../../common/shared_defs.h"
#include "fg_runtime_state.h"

namespace ce::streamline_runtime_policy {

struct ViewportRuntimeUpdate {
    bool shouldUpdate = false;
    bool active = false;
    int multiplier = 0;
    uint32_t generatedFrames = 0;
    uint32_t capabilityMax = 0;
};

struct GetStateRuntimeEvaluation {
    ViewportRuntimeUpdate update;
    bool suppressedFreshActivation = false;
};

struct CombinedRuntimeSignalUpdate {
    bool effectiveActive = false;
    int effectiveMultiplier = 0;
    bool deferredOffDuringStartupWindow = false;
    bool freshActivationEdge = false;
    bool shouldExtendStartupTransitionWindow = false;
};

struct ReflexFrameLimitForwarding {
    uint32_t frameLimitUs = 0;
    bool overrideApplied = false;
};

struct ObserverOnlyHeuristicCleanup {
    bool clearRecentTeardownGrace = false;
    bool seedRecentTeardownGrace = false;
    bool resetQueueChangeHeuristic = false;
    bool clearHeuristicFSR = false;
    bool clearNvidiaSmoothMotion = false;
};

inline bool IsDLSSGModeEnabled(uint32_t mode) {
    return mode != 0;
}

inline bool IsStreamlineReflexLowLatencyModeEnabled(int32_t mode) {
    return mode > 0;
}

inline bool IsStreamlineReflexFrameLimitActive(uint32_t frameLimitUs) {
    return frameLimitUs > 0;
}

inline bool IsStreamlineReflexPacingSignalActive(int32_t mode, uint32_t frameLimitUs) {
    return IsStreamlineReflexLowLatencyModeEnabled(mode) || IsStreamlineReflexFrameLimitActive(frameLimitUs);
}

inline char ToLowerAscii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

inline size_t StringLength(const char* value) {
    if (!value) {
        return 0;
    }

    size_t length = 0;
    while (value[length] != '\0') {
        ++length;
    }
    return length;
}

inline bool EqualsIgnoreCaseAscii(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    while (*lhs != '\0' && *rhs != '\0') {
        if (ToLowerAscii(*lhs) != ToLowerAscii(*rhs)) {
            return false;
        }
        ++lhs;
        ++rhs;
    }

    return *lhs == '\0' && *rhs == '\0';
}

inline bool HasPrefixIgnoreCaseAscii(const char* value, const char* prefix) {
    if (!value || !prefix) {
        return false;
    }

    while (*prefix != '\0') {
        if (*value == '\0' || ToLowerAscii(*value) != ToLowerAscii(*prefix)) {
            return false;
        }
        ++value;
        ++prefix;
    }
    return true;
}

inline bool HasSuffixIgnoreCaseAscii(const char* value, const char* suffix) {
    if (!value || !suffix) {
        return false;
    }

    const size_t valueLength = StringLength(value);
    const size_t suffixLength = StringLength(suffix);
    if (valueLength < suffixLength) {
        return false;
    }

    return EqualsIgnoreCaseAscii(value + valueLength - suffixLength, suffix);
}

inline const char* PathBaseName(const char* moduleNameOrPath) {
    if (!moduleNameOrPath) {
        return nullptr;
    }

    const char* baseName = moduleNameOrPath;
    for (const char* cursor = moduleNameOrPath; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            baseName = cursor + 1;
        }
    }
    return baseName;
}

inline bool IsStreamlineModuleNameForFeatureHooking(const char* moduleNameOrPath) {
    const char* baseName = PathBaseName(moduleNameOrPath);
    const size_t baseNameLength = StringLength(baseName);
    return baseNameLength > 7 && HasPrefixIgnoreCaseAscii(baseName, "sl.") &&
           HasSuffixIgnoreCaseAscii(baseName, ".dll");
}

inline bool IsStreamlineCoreModuleName(const char* moduleNameOrPath) {
    const char* baseName = PathBaseName(moduleNameOrPath);
    return EqualsIgnoreCaseAscii(baseName, "sl.interposer.dll") || EqualsIgnoreCaseAscii(baseName, "sl.common.dll");
}

inline bool ShouldInspectStreamlineModuleOnLoad(const char* moduleNameOrPath) {
    return IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath);
}

inline bool ShouldHookStreamlineCoreExportsOnLoad(const char* moduleNameOrPath) {
    return IsStreamlineCoreModuleName(moduleNameOrPath);
}

inline bool IsStreamlineDLSSGFeatureModuleName(const char* moduleNameOrPath) {
    return EqualsIgnoreCaseAscii(PathBaseName(moduleNameOrPath), "sl.dlss_g.dll");
}

inline bool IsStreamlineReflexFeatureModuleName(const char* moduleNameOrPath) {
    return EqualsIgnoreCaseAscii(PathBaseName(moduleNameOrPath), "sl.reflex.dll");
}

inline bool IsStreamlineDLSSGFeatureFunctionName(const char* functionName) {
    return EqualsIgnoreCaseAscii(functionName, "slDLSSGSetOptions") ||
           EqualsIgnoreCaseAscii(functionName, "slDLSSGGetState");
}

inline bool IsStreamlineReflexFeatureFunctionName(const char* functionName) {
    return EqualsIgnoreCaseAscii(functionName, "slReflexSleep") ||
           EqualsIgnoreCaseAscii(functionName, "slReflexSetOptions") ||
           EqualsIgnoreCaseAscii(functionName, "slReflexSetConstants");
}

inline bool ShouldHookStreamlineFeatureExportOnLoad(const char* functionName, const char* moduleNameOrPath) {
    if (IsStreamlineDLSSGFeatureFunctionName(functionName)) {
        return IsStreamlineDLSSGFeatureModuleName(moduleNameOrPath);
    }
    if (IsStreamlineReflexFeatureFunctionName(functionName)) {
        return IsStreamlineReflexFeatureModuleName(moduleNameOrPath);
    }
    return false;
}

inline bool ShouldSubstituteReturnedStreamlineFeatureWrapper(bool fallbackRequested, bool returnedFunctionIsWrapper,
                                                             bool originalCallableAvailable) {
    // A game can cache a function pointer returned from slGetFeatureFunction and
    // keep using it after Streamline tears down or repairs the feature export.
    // Returning CE's stable wrapper keeps that cached pointer observable while
    // the wrapper forwards through the current saved original/trampoline.
    return fallbackRequested && !returnedFunctionIsWrapper && originalCallableAvailable;
}

inline bool ShouldDeferStreamlineFeatureLookupDuringModuleLoad(bool loadedModuleIsStreamline) {
    // slInit loads plugin DLLs while Streamline's plugin manager is still
    // mutating internal state. Calling slGetFeatureFunction/slGetPluginFunction
    // from CE's loader notification can re-enter that manager and destabilize
    // the runtime. Direct export/IAT hooks are safe here; feature lookup is
    // retried from normal Streamline API calls such as slSetD3DDevice or the
    // app's own slGetFeatureFunction calls.
    return loadedModuleIsStreamline;
}

inline bool ShouldForwardSavedStreamlineOriginal(bool hasOriginal, bool validationAddressBelongsToLoadedModule) {
    return hasOriginal && validationAddressBelongsToLoadedModule;
}

inline bool IsRetryableLoadedModuleSnapshotError(uint32_t error) {
    // Toolhelp module snapshots can transiently fail while another thread is
    // loading/unloading a DLL. Windows reports that race as ERROR_BAD_LENGTH.
    return error == 24u;
}

inline bool ShouldOverrideStreamlineReflexFrameLimit(uint32_t targetIntervalUs) {
    return targetIntervalUs > 0;
}

inline ReflexFrameLimitForwarding ResolveStreamlineReflexFrameLimitForwarding(uint32_t incomingFrameLimitUs,
                                                                              uint32_t targetIntervalUs) {
    ReflexFrameLimitForwarding forwarding;
    forwarding.frameLimitUs =
        ShouldOverrideStreamlineReflexFrameLimit(targetIntervalUs) ? targetIntervalUs : incomingFrameLimitUs;
    forwarding.overrideApplied = forwarding.frameLimitUs != incomingFrameLimitUs;
    return forwarding;
}

inline int ResolveDLSSFGMultiplier(bool active, uint32_t requestedGeneratedFrames) {
    if (!active) {
        return 0;
    }

    const int multiplier = StreamlineGeneratedFramesToDLSSFGMultiplier(requestedGeneratedFrames);
    return multiplier > 0 ? multiplier : 2;
}

inline uint32_t ResolveDLSSFGGeneratedFrames(bool active, uint32_t requestedGeneratedFrames, int multiplier) {
    if (!active) {
        return 0;
    }

    return requestedGeneratedFrames > 0 ? requestedGeneratedFrames : DLSSFGMultiplierToGeneratedFrames(multiplier);
}

inline ViewportRuntimeUpdate BuildViewportRuntimeUpdateFromRequestedOptions(bool callSucceeded, bool hasOptions,
                                                                            uint32_t mode,
                                                                            uint32_t requestedGeneratedFrames,
                                                                            uint32_t capabilityMax) {
    ViewportRuntimeUpdate update;
    update.capabilityMax = capabilityMax;
    if (!callSucceeded || !hasOptions) {
        return update;
    }

    update.shouldUpdate = true;
    update.active = IsDLSSGModeEnabled(mode);
    update.multiplier = ResolveDLSSFGMultiplier(update.active, requestedGeneratedFrames);
    update.generatedFrames = ResolveDLSSFGGeneratedFrames(update.active, requestedGeneratedFrames, update.multiplier);
    return update;
}

inline bool ShouldApplyViewportRuntimeUpdateFromSetOptions(bool callSucceeded, bool setOptionsCallSuppressed) {
    // If CE intentionally swallowed a stale startup OFF request, Streamline never
    // observed that disable edge. Keep CE's own viewport/runtime reduction aligned
    // with the runtime's real live state instead of self-tearing the startup down
    // from local bookkeeping alone.
    return callSucceeded && !setOptionsCallSuppressed;
}

inline bool ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(bool callSucceeded, bool setOptionsCallSuppressed,
                                                                    uint32_t mode) {
    return ShouldApplyViewportRuntimeUpdateFromSetOptions(callSucceeded, setOptionsCallSuppressed) &&
           !IsDLSSGModeEnabled(mode);
}

inline ViewportRuntimeUpdate BuildViewportRuntimeUpdateFromGetState(
    bool callSucceeded, bool hasOptions, bool viewportWasActive, bool hasRuntimeFenceEvidence,
    bool suppressNewActivation, uint32_t mode, uint32_t requestedGeneratedFrames, uint32_t capabilityMax) {
    ViewportRuntimeUpdate update;
    update.capabilityMax = capabilityMax;
    if (!callSucceeded || !hasOptions) {
        return update;
    }

    const bool active = IsDLSSGModeEnabled(mode);
    if (active && !viewportWasActive && (suppressNewActivation || !hasRuntimeFenceEvidence)) {
        return update;
    }

    update.shouldUpdate = true;
    update.active = active;
    update.multiplier = ResolveDLSSFGMultiplier(active, requestedGeneratedFrames);
    update.generatedFrames = ResolveDLSSFGGeneratedFrames(active, requestedGeneratedFrames, update.multiplier);
    return update;
}

inline GetStateRuntimeEvaluation EvaluateViewportRuntimeUpdateFromGetState(
    bool callSucceeded, bool hasOptions, bool viewportWasActive, bool hasRuntimeFenceEvidence,
    bool suppressNewActivation, uint32_t mode, uint32_t requestedGeneratedFrames, uint32_t capabilityMax) {
    GetStateRuntimeEvaluation evaluation;
    evaluation.update =
        BuildViewportRuntimeUpdateFromGetState(callSucceeded, hasOptions, viewportWasActive, hasRuntimeFenceEvidence,
                                               suppressNewActivation, mode, requestedGeneratedFrames, capabilityMax);

    const bool attemptedFreshActivation = callSucceeded && hasOptions && IsDLSSGModeEnabled(mode) && !viewportWasActive;
    evaluation.suppressedFreshActivation =
        attemptedFreshActivation && suppressNewActivation && !evaluation.update.shouldUpdate;
    return evaluation;
}

inline bool ShouldClearAllViewportRuntimeStatesForGetStateDisable(bool callSucceeded, bool hasOptions,
                                                                  bool hasRuntimeFenceEvidence, uint32_t mode,
                                                                  uint32_t capabilityMax) {
    // A disabled GetState with both a valid DLSSG state read and runtime fence evidence is an
    // authoritative menu/runtime readback, not just an inactive sibling viewport probe.
    return callSucceeded && hasOptions && hasRuntimeFenceEvidence && capabilityMax > 0 && !IsDLSSGModeEnabled(mode);
}

inline bool ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(bool persistentSetOptionsBlock,
                                                                      bool startupTransitionWindowActive,
                                                                      ce::fg_runtime::RuntimeMode runtimeMode) {
    const bool runtimeStillInactive =
        runtimeMode == ce::fg_runtime::RuntimeMode::kStreamlineNoFG || runtimeMode == ce::fg_runtime::RuntimeMode::kOff;
    return runtimeStillInactive && (persistentSetOptionsBlock || startupTransitionWindowActive);
}

inline bool ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(bool blockUntilSafePostFSRBootstrapPath,
                                                                             bool safePostFSRBootstrapPath,
                                                                             ce::fg_runtime::RuntimeMode runtimeMode) {
    const bool runtimeStillInactive =
        runtimeMode == ce::fg_runtime::RuntimeMode::kStreamlineNoFG || runtimeMode == ce::fg_runtime::RuntimeMode::kOff;
    return runtimeStillInactive && blockUntilSafePostFSRBootstrapPath && !safePostFSRBootstrapPath;
}

inline bool ShouldArmStartupTransitionWindowOnFreshActiveSignal(bool active, bool previousSignal) {
    // The startup transition window is only for fresh activation churn around a
    // real handoff/enable edge. Keeping it refreshed by every later active
    // GetState/SetOptions poll causes startup-only OFF deferral to leak into
    // normal steady-state DLSS FG runtime operation.
    return active && !previousSignal;
}

inline bool ShouldPrimeStartupWindowOffExtensionLatch(bool effectiveActive, bool freshActivationEdge) {
    // The OFF-extension latch is intentionally one-shot per startup churn burst.
    // Prime it on a fresh activation edge (or a fresh authoritative handoff via
    // the dedicated caller path), but do not re-prime it on every later steady
    // active GetState/SetOptions poll or the startup window can keep extending
    // indefinitely during post-FSR OFF churn.
    return effectiveActive && freshActivationEdge;
}

inline CombinedRuntimeSignalUpdate ResolveCombinedRuntimeSignalUpdate(bool requestedActive, bool deferOffSignal,
                                                                      bool previousSignal, int requestedMultiplier) {
    CombinedRuntimeSignalUpdate update;
    update.deferredOffDuringStartupWindow = !requestedActive && deferOffSignal;
    update.effectiveActive = update.deferredOffDuringStartupWindow ? previousSignal : requestedActive;
    update.effectiveMultiplier = update.effectiveActive ? requestedMultiplier : 0;
    update.freshActivationEdge = requestedActive && !previousSignal;
    update.shouldExtendStartupTransitionWindow = update.deferredOffDuringStartupWindow && previousSignal;
    return update;
}

inline bool ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
    bool startupTransitionWindowActive, bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback,
    bool safePostFSRBootstrapPath, bool startupActivationPending, bool postSLActiveButUnconfirmed,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    if (startupTransitionWindowActive) {
        return true;
    }

    // A post-FSR comeback can still be half-armed after the literal startup window
    // has expired. Replaying the earlier OFF churn at that point can tear down the
    // same live comeback before PostSL has finished proving the recovered topology.
    // Explicit SetOptions(ON) is the strongest proof, but a GetState-only comeback
    // that already reached the repo's shared safe post-FSR bootstrap topology is
    // also far enough along that stale OFF churn must stay deferred until startup
    // protection really ends.
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool protectedPostSLWindow =
        postSLActiveButUnconfirmed || postSLConfirmedButStartupSettling || postSLConfirmedButRuntimeStateStabilizing;
    return hadFSRFGPhase && startupProtectedComebackProof && (startupActivationPending || protectedPostSLWindow) &&
           (!postSLConfirmedRendering || postSLConfirmedButStartupSettling ||
            postSLConfirmedButRuntimeStateStabilizing);
}

inline bool ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
    bool startupTransitionWindowActive, bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback,
    bool startupActivationPending, bool postSLActiveButUnconfirmed, bool postSLConfirmedRendering,
    bool postSLConfirmedButStartupSettling, bool postSLConfirmedButRuntimeStateStabilizing) {
    if (startupTransitionWindowActive) {
        return true;
    }

    // The same stale OFF churn can also collapse a fresh pure-DLSS startup after
    // expiry-driven PostSL activation but before the first confirmed render. GTA's
    // latest trace proves the first frame after settling ends can still carry the
    // same stale OFF burst, so keep only the stale-OFF guard alive for a short
    // post-settling stabilization window too.
    const bool protectedPostSLWindow =
        postSLActiveButUnconfirmed || postSLConfirmedButStartupSettling || postSLConfirmedButRuntimeStateStabilizing;
    return !hadFSRFGPhase && explicitSetOptionsActivationForCurrentComeback &&
           (startupActivationPending || protectedPostSLWindow) &&
           (!postSLConfirmedRendering || postSLConfirmedButStartupSettling ||
            postSLConfirmedButRuntimeStateStabilizing);
}

inline bool ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
    bool startupTransitionWindowActive, bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback,
    bool safePostFSRBootstrapPath, bool startupActivationPending, bool postSLActiveButUnconfirmed,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    return ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
               startupTransitionWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
               safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
               postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing) ||
           ShouldKeepOffChurnDeferredForStartupProtectedPureDLSSComeback(
               startupTransitionWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
               startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
               postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);
}

inline bool ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
    bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback, bool safePostFSRBootstrapPath,
    bool effectiveSignalActive, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    return hadFSRFGPhase && startupProtectedComebackProof && effectiveSignalActive &&
           !postSLConfirmedButStartupSettling && !postSLConfirmedButRuntimeStateStabilizing;
}

inline bool ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
    bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback, bool effectiveSignalActive,
    bool postSLConfirmedButStartupSettling, bool postSLConfirmedButRuntimeStateStabilizing) {
    return !hadFSRFGPhase && explicitSetOptionsActivationForCurrentComeback && effectiveSignalActive &&
           !postSLConfirmedButStartupSettling && !postSLConfirmedButRuntimeStateStabilizing;
}

inline bool ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
    bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback, bool safePostFSRBootstrapPath,
    bool effectiveSignalActive, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    return ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
               hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
               effectiveSignalActive, postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing) ||
           ShouldDropSuppressedOffChurnForStartupProtectedPureDLSSComeback(
               hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, effectiveSignalActive,
               postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);
}

inline constexpr uint32_t GetStartupProtectedOffChurnActiveProofUpdateThreshold() {
    return 3;
}

inline bool HasStartupProtectedOffChurnActiveProof(uint32_t consecutiveActiveRuntimeUpdatesAfterOffChurn) {
    return consecutiveActiveRuntimeUpdatesAfterOffChurn >= GetStartupProtectedOffChurnActiveProofUpdateThreshold();
}

inline bool ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
    bool startupProtectedOffChurnObserved, uint32_t consecutiveActiveRuntimeUpdatesAfterOffChurn,
    bool startupProtectedComebackProof, bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling) {
    // PostSL overlay submission proves CE can render through the recovered route,
    // but GTA can keep delivering stale Streamline OFF state after that render
    // path is already confirmed. Once startup-protected OFF churn has been seen,
    // require a few real active Streamline runtime updates after the last OFF
    // before accepting an inactive edge as a genuine disable.
    return startupProtectedOffChurnObserved &&
           !HasStartupProtectedOffChurnActiveProof(consecutiveActiveRuntimeUpdatesAfterOffChurn) &&
           startupProtectedComebackProof && postSLConfirmedRendering && !postSLConfirmedButStartupSettling;
}

inline bool ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
    bool requestedInactive, bool startupTransitionWindowActive, bool startupProtectedComebackProof,
    bool startupActivationPending, bool postSLActiveButUnconfirmed, bool postSLStartupActivationEntered,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    // A Streamline OFF edge that arrives after retained PostSL activation has
    // entered but before the first confirmed render is no longer the early stale
    // startup churn this guard was designed to swallow. In GTA this is the
    // settings-menu suspend edge; deferring it keeps SL half-active and can stall
    // the runtime. Only accept it once the literal startup window is gone, and
    // only for a proven comeback that is still unconfirmed.
    return requestedInactive && !startupTransitionWindowActive && startupProtectedComebackProof &&
           (startupActivationPending || postSLActiveButUnconfirmed) && postSLActiveButUnconfirmed &&
           postSLStartupActivationEntered && !postSLConfirmedRendering && !postSLConfirmedButStartupSettling &&
           !postSLConfirmedButRuntimeStateStabilizing;
}

inline bool ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
    bool requestedInactive, bool sourceWasSetOptions, bool postSLConfirmedRendering,
    bool startupActivationPending, bool postSLActiveButUnconfirmed, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    // Once PostSL has actually rendered through the current topology and no
    // startup/proof window is still settling, an explicit slDLSSGSetOptions(OFF)
    // is the app's real FG mode switch. Keep the early stale-OFF guard for
    // unconfirmed or still-stabilizing paths, but do not let it make the public
    // overlay state say DLSS FG after the app has intentionally disabled it.
    return requestedInactive && sourceWasSetOptions && postSLConfirmedRendering && !startupActivationPending &&
           !postSLActiveButUnconfirmed && !postSLConfirmedButStartupSettling &&
           !postSLConfirmedButRuntimeStateStabilizing;
}

inline bool ResolveCurrentComebackExplicitSetOptionsActivation(bool previousExplicitSetOptionsActivation,
                                                               bool effectiveSignalActive, bool freshActivationEdge,
                                                               bool explicitSetOptionsEnableSignal) {
    if (freshActivationEdge) {
        return explicitSetOptionsEnableSignal;
    }

    if (!effectiveSignalActive) {
        return false;
    }

    // Some runtimes surface the live comeback as active via GetState first and
    // only then deliver the explicit SetOptions(ON) for that same already-live
    // comeback. Preserve the stronger explicit provenance once that real enable
    // request arrives instead of leaving the comeback permanently classified as
    // GetState-only just because the shared active signal was already ON.
    if (explicitSetOptionsEnableSignal) {
        return true;
    }

    // Startup-window OFF churn can temporarily re-arm provisional GetState
    // suppression without changing the provenance of the current comeback.
    return previousExplicitSetOptionsActivation;
}

inline bool IsLiveFSRRuntimeHandoffSource(bool currentlyAuthoritativeFSRActive, bool currentRuntimeModeIsFSRFG) {
    return currentlyAuthoritativeFSRActive || currentRuntimeModeIsFSRFG;
}

inline bool ShouldPrepareForStreamlineEnableBeforeOriginalCall(bool requestedEnabled,
                                                               bool currentlyAuthoritativeFSRActive,
                                                               bool currentRuntimeModeIsFSRFG,
                                                               bool runtimeOwnsSwapchain) {
    return requestedEnabled && runtimeOwnsSwapchain &&
           IsLiveFSRRuntimeHandoffSource(currentlyAuthoritativeFSRActive, currentRuntimeModeIsFSRFG);
}

inline bool ShouldRequestStreamlineEnablePreparationOnReflexActivation(bool reflexActivating,
                                                                       bool currentlyAuthoritativeFSRActive,
                                                                       bool currentRuntimeModeIsFSRFG,
                                                                       bool runtimeOwnsSwapchain) {
    return reflexActivating && runtimeOwnsSwapchain &&
           IsLiveFSRRuntimeHandoffSource(currentlyAuthoritativeFSRActive, currentRuntimeModeIsFSRFG);
}

inline bool ShouldSuppressSetOptionsOffDuringStartupTransitionWindow(bool requestedDisabled,
                                                                     bool startupTransitionWindowActive) {
    return requestedDisabled && startupTransitionWindowActive;
}

inline bool ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(bool activationPending,
                                                                      bool postSLStartupActivationEntered) {
    return activationPending && !postSLStartupActivationEntered;
}

inline ObserverOnlyHeuristicCleanup ResolveObserverOnlyHeuristicCleanupForStreamlineSignalTransition(bool active) {
    ObserverOnlyHeuristicCleanup cleanup;
    cleanup.clearRecentTeardownGrace = active;
    cleanup.seedRecentTeardownGrace = !active;
    cleanup.resetQueueChangeHeuristic = true;
    cleanup.clearHeuristicFSR = true;
    cleanup.clearNvidiaSmoothMotion = true;
    return cleanup;
}

inline bool ShouldKeepPureObserverOnlyStreamlineBehavior(bool observerOnlyEnabled, bool observerPolicyOnlyEnabled) {
    return observerOnlyEnabled && !observerPolicyOnlyEnabled;
}

inline bool ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(bool observerOnlyEnabled,
                                                                    bool observerPolicyOnlyEnabled) {
    return observerOnlyEnabled && observerPolicyOnlyEnabled;
}

}  // namespace ce::streamline_runtime_policy
