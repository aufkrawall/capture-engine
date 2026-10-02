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
    // screenX/screenY identify the top-left of the cursor shape rather than
    // the cursor hotspot. DXGI Desktop Duplication uses this convention.
    kStatePositionIsShapeTopLeft = 1u << 4,
    // Pixel ownership for the source frame selected on this CFR tick. Keep
    // this separate from an actual CURSOR_SUPPRESSED observation: source
    // ownership follows the delayed video frame, while pointer visibility is
    // independently selected from the cursor timeline.
    kStateSourceEmbedded = 1u << 5,
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
        return IsValid() && (flags & kStateVisible) != 0 &&
               (flags & (kStateSuppressed | kStateSourceEmbedded)) == 0 && handle != 0;
    }
    bool PositionIsShapeTopLeft() const {
        return (flags & kStatePositionIsShapeTopLeft) != 0;
    }
    bool IsSourceEmbedded() const {
        return (flags & kStateSourceEmbedded) != 0;
    }
    void SetSourceEmbedded(bool embedded) {
        if (embedded) {
            flags |= kStateSourceEmbedded;
        } else {
            flags &= ~kStateSourceEmbedded;
        }
    }
};

// Source-owned pointer metadata associated with a QPC timestamp. In
// particular, DXGI Desktop Duplication reports shape-top-left coordinates and
// declares Position invalid whenever Visible is FALSE.
struct SourcePointerObservation {
    int64_t updateQpc = 0;
    int32_t screenX = 0;
    int32_t screenY = 0;
    bool valid = false;
    bool visible = false;
    bool embedded = false;
    bool positionValid = false;
    bool positionIsShapeTopLeft = false;
};

inline void ApplySourcePointerObservation(CaptureState* state, const SourcePointerObservation& observation) {
    if (!state || !observation.valid || observation.updateQpc <= 0) {
        return;
    }

    // The source timestamp is authoritative for this pointer observation. It
    // must not be retroactively associated with an unrelated desktop frame.
    state->associationQpc = observation.updateQpc;
    state->observedQpc = observation.updateQpc;
    const bool sampledVisible = (state->flags & kStateVisible) != 0 && state->handle != 0;
    const bool sampledSuppressed = (state->flags & kStateSuppressed) != 0;
    state->flags |= kStateValid;
    state->flags &= ~(kStateVisible | kStateSuppressed | kStateHandleVisibilityFallback |
                      kStatePositionIsShapeTopLeft | kStateSourceEmbedded);
    if (sampledSuppressed) {
        state->flags |= kStateSuppressed;
    }

    if (observation.embedded) {
        // Preserve the independently sampled cursor visibility/position for
        // the delayed timeline. The selected source frame owns the pixels for
        // this tick, so IsVisible() remains false until that ownership flag is
        // cleared after a hardware-plane transition.
        if (sampledVisible) {
            state->flags |= kStateVisible;
        }
        state->flags |= kStateSourceEmbedded;
        return;
    }
    if (!observation.visible) {
        // An authoritative hidden update must override the handle-based
        // DirectFlip fallback. The source explicitly says there is no separate
        // cursor to draw.
        return;
    }

    state->flags |= kStateVisible;
    if (observation.positionValid) {
        state->screenX = observation.screenX;
        state->screenY = observation.screenY;
        if (observation.positionIsShapeTopLeft) {
            state->flags |= kStatePositionIsShapeTopLeft;
        }
    }
}

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
        if (position != states_.begin()) {
            auto existing = std::prev(position);
            if (existing->associationQpc == state.associationQpc) {
                *existing = state;
                return;
            }
        }
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
