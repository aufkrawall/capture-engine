#pragma once

// Metering of the "UI tag record opportunity" diagnostics (streamline_hook_api.cpp).

#include <cstdint>

#include "common/logging/log_meter.h"

namespace ce::streamline_ui_tag_log {

// Heartbeat of one stream, in that stream's own calls: a steady title still gets a liveness line, at
// roughly one per 20 s per stream at 120 tag calls per second.
inline constexpr uint32_t kHeartbeatStride = 2400;

// The log stream of one tag call: its API, feature and viewport, and its shape. A game can tag several resource
// sets per frame on one viewport (GTA V Enhanced: 3, 4 and 6 tags in turn); a stream without the shape saw every
// alternation as a change and logged every call (25k lines, 47% of hook_debug.log in session 20261003_120641).
//
// `tagTypes` is the sequence of the call's tag types. A game can instead tag ONE resource per call and cycle
// the buffer role (The Witcher 3 Remastered: depth, motion vectors, UI and more as separate single-tag calls,
// every one with the same api, feature, viewport and numTags). The shape alone then read every call as a change
// of the one stream and logged all of them (21k of 25k lines in session 20261008_220437). The type picks the
// stream; lifecycle and extent stay in the change key, so a real change of one buffer's tag still logs.
inline uint64_t Stream(const char* api, uint32_t feature, uint32_t viewport, uint32_t numTags, uint32_t numInputs,
                       uint64_t tagTypes = 0) {
    return ce::log_meter::FieldKey(api, feature, viewport, numTags, numInputs, tagTypes);
}

}  // namespace ce::streamline_ui_tag_log
