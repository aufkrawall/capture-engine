#pragma once

#include "time_units.h"
#include "frame_timing_utils.h"

namespace ce::time {
inline QpcTicks SelectionSlot(QpcTicks origin, GridTick tick, QpcFrequency frequency, int fps) {
    return QpcTicks{ComputeIdealOutputQpcOnRationalGrid(origin.count(), tick.count(), frequency.count(), fps)};
}
inline FrameIndex NextOutputFrame(FrameIndex previous) {
    return FrameIndex{ComputeNextCfrFrameIndex(previous.count())};
}
inline Microseconds ScheduledElapsed(QpcTicks origin, QpcTicks scheduled, QpcFrequency frequency) {
    if (origin.count() <= 0 || frequency.count() <= 0 || scheduled.count() < origin.count())
        return Microseconds{-1};
    const int64_t delta = scheduled.count() - origin.count();
    return Microseconds{(delta / frequency.count()) * 1000000 +
                        ((delta % frequency.count()) * 1000000) / frequency.count()};
}
}  // namespace ce::time
