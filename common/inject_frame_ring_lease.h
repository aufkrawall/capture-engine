#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "shared_defs.h"

namespace ce {

// Marks one metadata slot complete, then publishes only the longest contiguous
// completed prefix. Frames are selected asynchronously by the encoder, so a
// later frame may finish before an earlier one; publishing readIndex directly
// for that later frame would let the producer reuse both metadata and textures
// that the encoder still owns.
inline void CompleteInjectFrameRingSlot(FrameRingBuffer& ring, uint32_t ringIndex) noexcept {
    const uint32_t readIndex = ring.readIndex.load(std::memory_order_acquire);
    const uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);
    const uint32_t depth = writeIndex - readIndex;
    const uint32_t offset = ringIndex - readIndex;
    if (depth == 0 || depth > static_cast<uint32_t>(FRAME_RING_SIZE) || offset >= depth) {
        return;
    }

    ring.slots[ringIndex % FRAME_RING_SIZE].valid.store(0, std::memory_order_release);

    for (uint32_t attempts = 0; attempts < static_cast<uint32_t>(FRAME_RING_SIZE); ++attempts) {
        uint32_t current = ring.readIndex.load(std::memory_order_acquire);
        const uint32_t published = ring.writeIndex.load(std::memory_order_acquire);
        if (current == published || ring.slots[current % FRAME_RING_SIZE].valid.load(std::memory_order_acquire) != 0) {
            break;
        }
        ring.readIndex.compare_exchange_weak(current, current + 1, std::memory_order_release,
                                             std::memory_order_acquire);
    }
}

class InjectFrameRingLeaseState;

class InjectFrameRingLease final {
public:
    InjectFrameRingLease() = default;
    InjectFrameRingLease(const InjectFrameRingLease&) = delete;
    InjectFrameRingLease& operator=(const InjectFrameRingLease&) = delete;

    InjectFrameRingLease(InjectFrameRingLease&& other) noexcept
        : state_(std::move(other.state_)),
          ringIndex_(other.ringIndex_) {
        other.ringIndex_ = 0;
    }

    InjectFrameRingLease& operator=(InjectFrameRingLease&& other) noexcept {
        if (this != &other) {
            Reset();
            state_ = std::move(other.state_);
            ringIndex_ = other.ringIndex_;
            other.ringIndex_ = 0;
        }
        return *this;
    }

    ~InjectFrameRingLease() {
        Reset();
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(state_);
    }

    uint32_t RingIndex() const noexcept {
        return ringIndex_;
    }

    void Reset() noexcept;

private:
    friend class InjectFrameRingLeaseState;

    InjectFrameRingLease(std::shared_ptr<InjectFrameRingLeaseState> state, uint32_t ringIndex) noexcept
        : state_(std::move(state)),
          ringIndex_(ringIndex) {}

    std::shared_ptr<InjectFrameRingLeaseState> state_;
    uint32_t ringIndex_ = 0;
};

// The state can safely outlive the ingest thread. Detach is available for final
// shared-memory teardown; late frame destruction then becomes a no-op instead
// of dereferencing an unmapped cross-process ring.
class InjectFrameRingLeaseState final : public std::enable_shared_from_this<InjectFrameRingLeaseState> {
public:
    explicit InjectFrameRingLeaseState(FrameRingBuffer* ring) noexcept : ring_(ring) {}

    InjectFrameRingLease Acquire(uint32_t ringIndex) {
        return InjectFrameRingLease(shared_from_this(), ringIndex);
    }

    void Complete(uint32_t ringIndex) noexcept {
        FrameRingBuffer* ring = ring_.load(std::memory_order_acquire);
        if (ring) {
            CompleteInjectFrameRingSlot(*ring, ringIndex);
        }
    }

    void Detach() noexcept {
        ring_.store(nullptr, std::memory_order_release);
    }

private:
    std::atomic<FrameRingBuffer*> ring_{nullptr};
};

inline void InjectFrameRingLease::Reset() noexcept {
    if (state_) {
        state_->Complete(ringIndex_);
        state_.reset();
    }
    ringIndex_ = 0;
}

}  // namespace ce
