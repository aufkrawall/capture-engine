#pragma once

#include <stdint.h>
#include <limits>

// Exact rational CFR output grid.
//
// The recording's packet timestamps are exact: slot n is n/fps seconds and the
// CFR audio target follows it sample-exactly, while audio samples themselves are
// placed by QPC. The real-time slot grid must therefore advance by exactly
// qpcFrequency/fps per slot. That quotient is rarely an integer (10 MHz / 120 =
// 83333.33 ticks), and stepping the truncated interval ran the grid fast by
// fraction/interval: 4 ppm at 60/120 fps, 6.4 ppm at 144, 16 ppm at 240 and
// 28 ppm at 360 fps. Video content then fell behind the QPC-placed audio by
// ~14-100 ms per recorded hour, and the audio consumer crept ahead of the
// capture edge by the same amount. Slot n now lies at floor(n * qpcFrequency / fps)
// ticks after the grid origin; the truncated interval remains valid only as a
// duration (tolerances, waits, frame budgets), never as a position stride.

namespace ce::cfr_grid {

// Offset of slot `ticks` from the grid origin: floor(ticks * qpcFrequency / fps).
// Split into whole seconds so the multiplication stays exact for any QPC rate.
// Returns false on invalid input or int64 overflow.
inline bool TryGetSlotOffsetQpc(uint64_t ticks, int64_t qpcFrequency, int fps, int64_t* offsetQpc) {
    if (!offsetQpc || qpcFrequency <= 0 || fps <= 0) {
        return false;
    }
    const uint64_t frequency = static_cast<uint64_t>(qpcFrequency);
    const uint64_t rate = static_cast<uint64_t>(fps);
    const uint64_t wholeSeconds = ticks / rate;
    const uint64_t remainderTicks = ticks % rate;
    const uint64_t maxOffset = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (wholeSeconds > maxOffset / frequency) {
        return false;
    }
    const uint64_t wholeQpc = wholeSeconds * frequency;
    const uint64_t fractionQpc = (remainderTicks * frequency) / rate;
    if (wholeQpc > maxOffset - fractionQpc) {
        return false;
    }
    *offsetQpc = static_cast<int64_t>(wholeQpc + fractionQpc);
    return true;
}

// Absolute QPC of slot `ticks` on a grid anchored at originQpc, or fallbackQpc
// when the position cannot be represented.
inline int64_t GetSlotQpc(int64_t originQpc, uint64_t ticks, int64_t qpcFrequency, int fps, int64_t fallbackQpc) {
    int64_t offsetQpc = 0;
    if (!TryGetSlotOffsetQpc(ticks, qpcFrequency, fps, &offsetQpc) ||
        originQpc > std::numeric_limits<int64_t>::max() - offsetQpc) {
        return fallbackQpc;
    }
    return originQpc + offsetQpc;
}

// Index of the latest slot whose position is at or before elapsedQpc: the
// largest n with floor(n * qpcFrequency / fps) <= elapsedQpc, which is
// floor(((elapsedQpc + 1) * fps - 1) / qpcFrequency). Exact inverse of
// TryGetSlotOffsetQpc; negative elapsed time maps to slot 0.
inline uint64_t GetSlotIndexAtOrBefore(int64_t elapsedQpc, int64_t qpcFrequency, int fps) {
    if (elapsedQpc < 0 || qpcFrequency <= 0 || fps <= 0) {
        return 0;
    }
    const uint64_t frequency = static_cast<uint64_t>(qpcFrequency);
    const uint64_t rate = static_cast<uint64_t>(fps);
    const uint64_t shifted = static_cast<uint64_t>(elapsedQpc) + 1;
    const uint64_t wholeSeconds = shifted / frequency;
    const uint64_t remainderQpc = shifted % frequency;
    if (remainderQpc == 0) {
        return wholeSeconds * rate - 1;
    }
    return wholeSeconds * rate + (remainderQpc * rate - 1) / frequency;
}

// Incremental form for wake deadlines: returns the stride from slot k to k+1
// (floor(qpcFrequency / fps) or one more) and carries the sub-tick fraction in
// `remainder`. Starting from remainder 0 at the origin, k calls sum to exactly
// floor(k * qpcFrequency / fps).
inline int64_t NextSlotIntervalQpc(int64_t qpcFrequency, int fps, int64_t& remainder) {
    if (qpcFrequency <= 0 || fps <= 0) {
        remainder = 0;
        return 0;
    }
    int64_t interval = qpcFrequency / fps;
    remainder += qpcFrequency % fps;
    if (remainder >= fps) {
        remainder -= fps;
        ++interval;
    }
    return interval;
}

}  // namespace ce::cfr_grid
