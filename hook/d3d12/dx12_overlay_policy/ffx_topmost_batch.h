#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ce::dx12_overlay_policy {

struct FinalECLBatchSignature {
    uintptr_t callSite = 0;
    uintptr_t queueIdentity = 0;
    uint32_t ordinal = 0;
};

inline bool IsValidFinalECLBatchSignature(const FinalECLBatchSignature& signature) {
    return signature.callSite != 0 && signature.queueIdentity != 0 && signature.ordinal != 0;
}

inline bool SameFinalECLBatchSignature(const FinalECLBatchSignature& left,
                                       const FinalECLBatchSignature& right) {
    return left.callSite == right.callSite && left.queueIdentity == right.queueIdentity &&
           left.ordinal == right.ordinal;
}

inline uint32_t AdvanceFinalECLBatchSignatureStability(const FinalECLBatchSignature& previous,
                                                       uint32_t previousStableFrames,
                                                       const FinalECLBatchSignature& observed) {
    if (!IsValidFinalECLBatchSignature(observed)) {
        return 0;
    }
    if (!SameFinalECLBatchSignature(previous, observed)) {
        return 1;
    }
    return previousStableFrames == std::numeric_limits<uint32_t>::max()
               ? previousStableFrames
               : previousStableFrames + 1;
}

inline bool ShouldAppendTopmostOverlayToFinalECLBatch(
    bool routeEligible, uint32_t stableFrames, const FinalECLBatchSignature& target,
    uintptr_t currentCallSite, uintptr_t currentQueueIdentity, uint32_t currentOrdinal,
    size_t commandListCount, size_t combinedBatchCapacity) {
    return routeEligible && stableFrames >= 2 && IsValidFinalECLBatchSignature(target) &&
           currentCallSite == target.callSite && currentQueueIdentity == target.queueIdentity &&
           currentOrdinal == target.ordinal && commandListCount != 0 &&
           commandListCount < combinedBatchCapacity;
}

// One ExecuteCommandLists call on the presenter thread between two of its Presents.
struct PresenterBatch {
    uintptr_t callSite = 0;
    uintptr_t queueIdentity = 0;
    bool fromPresentingRuntime = false;  // the call site lies in the frame generation runtime image that presents
};

// The batch the route appends CE's overlay to on the next Presents: the last one the presenting runtime image itself
// submitted, which composes every output. Overlays hooked into the Present chain (Steam, OptiScaler's own UI, a
// Present-time effect) submit after it and only while they have something to draw: appending to the last batch of
// all lost the overlay on the output where such a layer stopped (Talos, session 20261024_015207: Steam's and
// OptiScaler's start-up messages, 7 outputs). Those layers now draw above CE while they draw. A Present without a
// runtime batch has no anchor. Without a known runtime image the last batch of the Present stays the anchor.
inline FinalECLBatchSignature ChooseTopmostAnchorBatch(const PresenterBatch* batches, size_t count,
                                                       bool runtimeImageKnown) {
    if (!batches || count == 0) {
        return {};
    }
    size_t anchor = count;
    for (size_t i = count; i-- > 0;) {
        if (!runtimeImageKnown || batches[i].fromPresentingRuntime) {
            anchor = i;
            break;
        }
    }
    if (anchor == count) {
        return {};
    }
    FinalECLBatchSignature signature{batches[anchor].callSite, batches[anchor].queueIdentity, 0};
    for (size_t i = 0; i <= anchor; ++i) {
        if (batches[i].callSite == signature.callSite) {
            ++signature.ordinal;
        }
    }
    return signature;
}

// AMD presents the first outputs after frame generation starts from the game's own Present, on the game thread, until
// its presenter thread takes over (session 20261024_015207: about 8 outputs, twice). A route learned on the game thread
// no longer matches once presentation moves, and the outputs of the frames it already owned had no overlay. Ownership
// therefore waits until the runtime presents from a thread of its own; until then the baseline keeps the overlay.
inline bool HasCompletedNoCallbackTopmostActivation(bool routeReady, bool previousAppendSucceeded,
                                                     bool inlineCompletionObserved, bool presentedOffTheGameThread) {
    return routeReady && previousAppendSucceeded && inlineCompletionObserved && presentedOffTheGameThread;
}

// A route learned on one presenting thread is relearned on another: ordinals and the trace are per thread.
inline bool IsNewTopmostPresentation(bool swapChainChanged, bool queueChanged, bool presenterThreadChanged) {
    return swapChainChanged || queueChanged || presenterThreadChanged;
}

inline bool ShouldGrantNoCallbackTopmostOwnership(bool activationComplete, bool uiBaselineRetired) {
    return activationComplete && uiBaselineRetired;
}

inline bool ShouldRenderAppCallbackTopmostOverlay(bool routeArmed) {
    return routeArmed;
}

inline bool ShouldRetireWarmFSRRendererForPresentationChange(bool hadPresentation,
                                                             bool hasReplacementPresentation,
                                                             bool presentationIdentityChanged) {
    // Callback/no-callback routing changes temporarily release the topmost tracker's COM references while the
    // same FFX context remains live. Retiring there rebuilds PSOs/upload pools on a later Present. Only a real
    // live-identity replacement (or the separate authoritative context teardown) retires renderer resources.
    return hadPresentation && hasReplacementPresentation && presentationIdentityChanged;
}

inline bool ShouldSampleFrameTimingFromFFXPresentCallback(bool runtimeOwnsNativeFSRPresentation,
                                                          bool callbackYieldsToTopmostRoute,
                                                          bool presentInterceptedBelowForeignChain) {
    // A deep-body Present interception observes the exact displayed-output boundary after the FFX callback.
    // Sampling both sites doubles FPS and alternates tiny/normal frame times. Retain the callback only as the
    // fallback for runtime-owned presentation that does not re-enter CE through DXGI.
    return runtimeOwnsNativeFSRPresentation && !callbackYieldsToTopmostRoute &&
           !presentInterceptedBelowForeignChain;
}

inline bool ShouldYieldFFXPresentCallbackToTopmostRoute(
    bool nativeNoCallbackComposition, bool belowForeignTopmostSubmitProven,
    bool completedNoCallbackTopmostBatch) {
    // App-callback FSR yields only after the deep Present route has submitted successfully in this routing epoch.
    // No-callback FSR needs the stronger learned final-batch completion proof before retiring either baseline.
    return nativeNoCallbackComposition ? completedNoCallbackTopmostBatch
                                       : belowForeignTopmostSubmitProven;
}

}  // namespace ce::dx12_overlay_policy
