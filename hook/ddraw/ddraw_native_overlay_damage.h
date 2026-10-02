/**
 * Damage state for an overlay drawn into a DirectDraw surface by the game's
 * native Direct3D device.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ddraw_present_policy.h"

namespace ce::ddraw_native_overlay {

using ce::ddraw_present_policy::Rect;

enum class State { Absent, Current, Repairable, Unsafe };

// Native pixels have no CPU backdrop. Exact application writes can still be
// repaired safely because every pixel in their rectangles is known to have
// replaced the old overlay. Unknown writes cannot: blending the full sprite
// would apply its translucent pixels over themselves outside the actual write.
class DamageTracker {
public:
    static constexpr size_t kMaxRepairRects = 8;

    void SetCurrent(const Rect& overlayBounds) {
        if (overlayBounds.IsEmpty()) {
            Clear();
            return;
        }
        bounds = overlayBounds;
        repairs = {};
        repairCount = 0;
        state = State::Current;
    }

    void Clear() {
        bounds = {};
        repairs = {};
        repairCount = 0;
        state = State::Absent;
    }

    void RecordWrite(bool exact, const Rect& changedRect) {
        if (state == State::Absent)
            return;
        if (!exact) {
            repairs = {};
            repairCount = 0;
            state = State::Unsafe;
            return;
        }

        Rect repair = ce::ddraw_present_policy::IntersectRect(bounds, changedRect);
        if (repair.IsEmpty())
            return;
        if (Contains(repair, bounds)) {
            // All old native pixels are gone, so the ordinary CPU composite can
            // establish a fresh full backdrop.
            Clear();
            return;
        }
        if (state == State::Unsafe)
            return;
        state = State::Repairable;

        // Repeatedly coalesce rectangular unions. A new strip can bridge two
        // existing repairs, so one merge is not necessarily the fixed point.
        for (size_t i = 0; i < repairCount;) {
            const Rect merged = ce::ddraw_present_policy::UnionRect(repairs[i], repair);
            if (!UnionHasNoGap(repairs[i], repair, merged)) {
                ++i;
                continue;
            }
            repair = merged;
            RemoveAt(i);
            i = 0;
        }
        if (repairCount >= repairs.size()) {
            repairs = {};
            repairCount = 0;
            state = State::Unsafe;
            return;
        }
        repairs[repairCount++] = repair;
    }

    State CopyRepairs(Rect* output, size_t capacity, size_t& outputCount) const {
        outputCount = 0;
        if (state != State::Repairable)
            return state;
        if (!output || repairCount > capacity)
            return State::Unsafe;
        std::copy_n(repairs.begin(), repairCount, output);
        outputCount = repairCount;
        return state;
    }

    void CompleteRepair(const Rect& repairedRect) {
        if (state != State::Repairable)
            return;
        for (size_t i = 0; i < repairCount; ++i) {
            if (SameRect(repairs[i], repairedRect)) {
                RemoveAt(i);
                if (repairCount == 0)
                    state = State::Current;
                return;
            }
        }
    }

private:
    static bool SameRect(const Rect& a, const Rect& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }

    static bool Contains(const Rect& outer, const Rect& inner) {
        return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
               outer.bottom >= inner.bottom;
    }

    static int64_t Area(const Rect& rect) {
        if (rect.IsEmpty())
            return 0;
        return static_cast<int64_t>(rect.right - rect.left) *
               static_cast<int64_t>(rect.bottom - rect.top);
    }

    static bool UnionHasNoGap(const Rect& a, const Rect& b, const Rect& merged) {
        const Rect overlap = ce::ddraw_present_policy::IntersectRect(a, b);
        return Area(merged) == Area(a) + Area(b) - Area(overlap);
    }

    void RemoveAt(size_t index) {
        for (size_t move = index + 1; move < repairCount; ++move)
            repairs[move - 1] = repairs[move];
        repairs[--repairCount] = {};
    }

    Rect bounds = {};
    std::array<Rect, kMaxRepairRects> repairs = {};
    size_t repairCount = 0;
    State state = State::Absent;
};

}  // namespace ce::ddraw_native_overlay
