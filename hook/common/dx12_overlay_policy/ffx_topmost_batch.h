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

inline bool ShouldYieldFFXPresentCallbackToTopmostRoute(
    bool nativeNoCallbackComposition, bool belowForeignTopmostSubmitProven,
    bool completedNoCallbackTopmostBatch) {
    // App-callback FSR yields only after the deep Present route has submitted successfully in this routing epoch.
    // No-callback FSR needs the stronger learned final-batch completion proof before retiring either baseline.
    return nativeNoCallbackComposition ? completedNoCallbackTopmostBatch
                                       : belowForeignTopmostSubmitProven;
}

}  // namespace ce::dx12_overlay_policy
