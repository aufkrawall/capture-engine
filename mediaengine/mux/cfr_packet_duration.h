#pragma once

#include <algorithm>
#include <cstdint>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace ce::mux {

// The longest duration, in CFR slots, a video packet may carry into the muxer.
// Two slots absorb codec durations of 0 or 2 from rounding; anything longer
// is a bogus codec value. An AV1 encoder packet that holds committed
// show-existing slots it can no longer be shown for (av1_show_existing.h)
// carries them explicitly: cutting them ended the video before the CFR
// timeline and the audio when it was the recording's last packet.
inline int64_t CfrVideoPacketDurationCeilingSlots(int64_t heldSlots) {
    return 2 + std::max<int64_t>(0, heldSlots);
}

// A CFR video packet's duration, already in the stream time base, capped at
// that ceiling. Both video writers (WriteFrame and the finalize flush) use it.
inline int64_t ClampCfrVideoPacketDuration(int64_t duration, int64_t heldSlots, AVRational codecTimeBase,
                                           AVRational streamTimeBase) {
    const int64_t ceiling = std::max<int64_t>(
        2, av_rescale_q(CfrVideoPacketDurationCeilingSlots(heldSlots), codecTimeBase, streamTimeBase));
    return std::min(duration, ceiling);
}

}  // namespace ce::mux
