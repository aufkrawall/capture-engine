#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>

namespace ce::cursor {

enum CaptureStateFlags : uint32_t {
    kStateValid = 1u << 0,
    kStateVisible = 1u << 1,
    kStateSuppressed = 1u << 2,
    kStateHandleVisibilityFallback = 1u << 3,
};

// A plain-data snapshot shared across the capture EXE / media-engine DLL ABI.
// associationQpc identifies the source-content time this cursor belongs to;
// observedQpc records when Windows was queried and is diagnostics-only.
struct CaptureState {
    uint64_t handle = 0;
    int64_t associationQpc = 0;
    int64_t observedQpc = 0;
    int32_t screenX = 0;
    int32_t screenY = 0;
    int32_t captureLeft = 0;
    int32_t captureTop = 0;
    uint32_t captureWidth = 0;
    uint32_t captureHeight = 0;
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
    uint32_t dpi = 96;
    uint32_t flags = 0;

    bool IsValid() const {
        return (flags & kStateValid) != 0;
    }
    bool IsVisible() const {
        return IsValid() && (flags & kStateVisible) != 0 && (flags & kStateSuppressed) == 0 && handle != 0;
    }
};

// Bounded, timestamp-ordered cursor history. Capture callbacks publish here,
// while the CFR scheduler selects the snapshot belonging to a source-content
// time. Out-of-order callback completion is intentionally supported.
class Timeline {
public:
    explicit Timeline(size_t capacity = 512) : capacity_(std::max<size_t>(capacity, 1)) {}

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.clear();
    }

    void Publish(const CaptureState& state) {
        if (!state.IsValid() || state.associationQpc <= 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto position = std::upper_bound(
            states_.begin(), states_.end(), state.associationQpc,
            [](int64_t timestamp, const CaptureState& candidate) { return timestamp < candidate.associationQpc; });
        states_.insert(position, state);
        while (states_.size() > capacity_) {
            states_.pop_front();
        }
    }

    bool SelectAtOrBefore(int64_t targetQpc, CaptureState* result) const {
        if (!result || targetQpc <= 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto position = std::upper_bound(
            states_.begin(), states_.end(), targetQpc,
            [](int64_t timestamp, const CaptureState& candidate) { return timestamp < candidate.associationQpc; });
        if (position == states_.begin()) {
            return false;
        }
        *result = *std::prev(position);
        return true;
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<CaptureState> states_;
};

}  // namespace ce::cursor
