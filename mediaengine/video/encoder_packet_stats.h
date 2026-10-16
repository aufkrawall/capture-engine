#pragma once

#include <cstdint>

// Per-packet type tracking for B-frame quality diagnostics (FINAL PACKET STATS).
struct EncoderPacketStats {
    int64_t keyframeBytes = 0;
    int keyframeCount = 0;
    int64_t refBytes = 0;  // Non-key reference frames (P-frames)
    int refCount = 0;
    int64_t sefBytes = 0;  // show_existing_frame (<=10 bytes)
    int sefCount = 0;
    int64_t bframeBytes = 0;  // Small non-keyframe packets (B-frames)
    int bframeCount = 0;
    int totalPackets = 0;

    void Reset() {
        *this = {};
    }
};
