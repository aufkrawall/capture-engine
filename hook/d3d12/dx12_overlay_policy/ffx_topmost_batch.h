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

// The code that presents the outputs: the frame generation runtime (AMD's FidelityFX image) and, where the swapchain it
// presents to is another component's proxy, that nested presenter - the module that created the native swapchain and
// issues its Present from inside the runtime's (GTA V Enhanced: Streamline's sl.dlss_g.dll, DLSS-G off, session
// 20261112_013802). AMD then composes into the proxy's buffers on a queue of its own, and only the nested presenter's
// copy writes the real back buffer, on the swapchain's queue, inside AMD's Present call.
struct PresenterImages {
    uintptr_t runtimeBase = 0;
    uintptr_t runtimeEnd = 0;
    uintptr_t nestedBase = 0;  // 0: the runtime presents the native swapchain itself
    uintptr_t nestedEnd = 0;
};

// The Present's originator (the module that calls into the chain below it) is a nested presenter when it is not the
// runtime and it created the native swapchain itself. An overlay wrapping the swapchain created none, and the runtime
// presenting its own swapchain is no nested presenter. Module identities; 0 is unknown.
inline bool IsNestedSwapchainPresenter(uintptr_t originatorModule, uintptr_t creatorModule,
                                       bool originatorInRuntimeImage) {
    return originatorModule != 0 && originatorModule == creatorModule && !originatorInRuntimeImage;
}

enum class PresenterImage : uint8_t { kNone, kRuntime, kNested };

struct PresenterFrame {
    PresenterImage image = PresenterImage::kNone;
    uintptr_t address = 0;
};

// The nearest return address on a submission's stack (innermost first) inside either presenting image: whose code
// submitted it, past any queue proxy that relayed it. A Present-chain layer's submission finds the call into the layer
// below the image instead (the runtime's or the nested presenter's Present site), which tells it apart.
inline PresenterFrame NearestPresenterFrame(const uintptr_t* frames, size_t count, const PresenterImages& images) {
    for (size_t i = 0; frames && i < count; ++i) {
        const uintptr_t address = frames[i];
        if (address >= images.runtimeBase && address < images.runtimeEnd)
            return {PresenterImage::kRuntime, address};
        if (images.nestedBase != 0 && address >= images.nestedBase && address < images.nestedEnd)
            return {PresenterImage::kNested, address};
    }
    return {};
}

// One ExecuteCommandLists call on the presenter thread between two of its Presents.
struct PresenterBatch {
    uintptr_t callSite = 0;
    uintptr_t queueIdentity = 0;
    bool fromPresentingRuntime = false;
    bool relayed = false;  // callSite is the return address in the runtime behind a queue proxy
    // The batch runs on the queue the presented swapchain's buffers belong to (its create or ResizeBuffers1 queue).
    // CE's draw writes those buffers, and D3D12 removes the device when any other queue writes them: GTA V Enhanced
    // with ReShade (gtadlssfgtofsrfgcrash) died on CE's first draw inside AMD's batch on a second DIRECT queue.
    bool ownsBackBuffers = true;
    bool fromNestedPresenter = false;  // callSite is the nearest return address in the nested presenter's image
};

// The batch the route appends CE's overlay to on the next Presents: the last one the presenting runtime image itself
// submitted on the swapchain's own queue, which composes every output. Overlays hooked into the Present chain (Steam,
// OptiScaler's own UI, a Present-time effect) submit after it and only while they have something to draw: appending
// to the last batch of all lost the overlay on the output where such a layer stopped (Talos, session 20261024_015207:
// Steam's and OptiScaler's start-up messages, 7 outputs). Those layers now draw above CE while they draw. A Present
// without a runtime batch on the swapchain's queue has no anchor; runtime work on any other queue (compute, copy, a
// second DIRECT queue) never writes the back buffers and can never carry CE's draw. When a queue proxy obscures the
// submitter, use the runtime's return address. Direct and proven relayed submissions share the same chronological
// order. An overlay submitting inside the Present chain has the runtime's Present return address on its stack, so it
// cannot establish an anchor. A nested presenter (PresenterImages) composes inside that call: its own submission on
// the swapchain's queue is the last write of the runtime's output and anchors like the runtime's; a layer below it
// (ReShade's effects, DXGI's own work) finds the nested presenter's Present site instead and never does.
inline FinalECLBatchSignature ChooseTopmostAnchorBatch(const PresenterBatch* batches, size_t count,
                                                       bool runtimeImageKnown, uintptr_t runtimePresentSite = 0,
                                                       size_t* anchorIndex = nullptr,
                                                       uintptr_t nestedPresentSite = 0) {
    if (!batches || count == 0) {
        return {};
    }
    size_t anchor = count;
    for (size_t i = count; i-- > 0;) {
        if (!batches[i].ownsBackBuffers)
            continue;
        const bool runtimeComposition =
            batches[i].fromPresentingRuntime &&
            (!batches[i].relayed || (runtimePresentSite != 0 && batches[i].callSite != runtimePresentSite));
        const bool nestedComposition =
            batches[i].fromNestedPresenter && nestedPresentSite != 0 && batches[i].callSite != nestedPresentSite;
        if (!runtimeImageKnown || runtimeComposition || nestedComposition) {
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
    if (anchorIndex)
        *anchorIndex = anchor;
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
