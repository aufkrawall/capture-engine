#include "media_gpu_priority_claims.h"

#include <algorithm>
#include <utility>

namespace ce::media_gpu_priority {

GpuSchedulingPriorityClaims::GpuSchedulingPriorityClaims(SetProcessClassFn setClass) : setClass_(std::move(setClass)) {}

ClaimResult GpuSchedulingPriorityClaims::Claim(const void* claimant, std::optional<int> classBeforeChange) {
    ClaimResult result;
    if (std::find(claimants_.begin(), claimants_.end(), claimant) == claimants_.end()) {
        result.started = claimants_.empty();
        if (result.started) {
            priorKnown_ = classBeforeChange.has_value();
            priorClass_ = classBeforeChange.value_or(-1);
        }
        claimants_.push_back(claimant);
        result.added = true;
    }
    result.priorKnown = priorKnown_;
    result.priorClass = priorClass_;
    result.claimants = static_cast<int>(claimants_.size());
    return result;
}

ReleaseResult GpuSchedulingPriorityClaims::Release(const void* claimant) {
    ReleaseResult result;
    const auto held = std::find(claimants_.begin(), claimants_.end(), claimant);
    if (held != claimants_.end()) {
        claimants_.erase(held);
        result.released = true;
    }
    result.claimants = static_cast<int>(claimants_.size());
    if (!result.released || !claimants_.empty()) {
        return result;
    }

    // The set ends with its last claim whatever the write reports: a later claim starts a new set and reads again.
    const bool priorKnown = priorKnown_;
    const int priorClass = priorClass_;
    priorKnown_ = false;
    priorClass_ = -1;
    if (!priorKnown) {
        result.priorUnknown = true;
        return result;
    }
    result.attempted = true;
    result.restoredClass = priorClass;
    result.status = setClass_(priorClass);
    result.restored = result.status >= 0;
    return result;
}

}  // namespace ce::media_gpu_priority
