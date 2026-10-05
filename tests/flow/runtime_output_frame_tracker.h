#pragma once

#include <cstdint>

namespace ce::flow {

// The caller serializes observations; presenter addresses may be reused across drained lifetimes.
class RuntimeOutputFrameTracker {
public:
    enum class Observation { kFirstInLifetime, kConsistent, kMismatch };

    Observation Observe(const void* presenter, uint64_t lifetime, int64_t offset) {
        if (!haveOffset_ || presenter != presenter_ || lifetime != lifetime_) {
            presenter_ = presenter;
            lifetime_ = lifetime;
            offset_ = offset;
            haveOffset_ = true;
            return Observation::kFirstInLifetime;
        }
        return offset == offset_ ? Observation::kConsistent : Observation::kMismatch;
    }

    int64_t Offset() const { return offset_; }

private:
    const void* presenter_ = nullptr;
    uint64_t lifetime_ = 0;
    int64_t offset_ = 0;
    bool haveOffset_ = false;
};

}  // namespace ce::flow
