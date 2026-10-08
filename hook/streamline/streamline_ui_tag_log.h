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
// `tagKinds` is the fold of the call's tags' kinds (see TagKinds). A game can instead tag ONE resource per call
// and cycle the buffer role (The Witcher 3 Remastered: depth, motion vectors, UI and more as separate single-tag
// calls, every one with the same api, feature, viewport and numTags). The shape alone then read every call as a
// change of the one stream and logged all of them (21k of 25k lines in session 20261008_220437). The kind picks
// the stream; lifecycle and extent stay in the change key, so a real change of one buffer's tag still logs.
inline uint64_t Stream(const char* api, uint32_t feature, uint32_t viewport, uint32_t numTags, uint32_t numInputs,
                       uint64_t tagKinds = 0) {
    return ce::log_meter::FieldKey(api, feature, viewport, numTags, numInputs, tagKinds);
}

// Folds one tag into the kinds fingerprint: its buffer type and whether it carries a resource. The same type is
// sent twice per frame, once with the real resource and once with none (a null resource clears the tag), and
// those two calls alternate for the whole run (20261008_221807: 930 of the 955 lines left after the type
// split were types 0 and 1 alternating 1280x720/resource against 0x0/null).
inline uint64_t TagKinds(uint64_t seed, uint32_t type, bool hasResource) {
    return ce::log_meter::FieldKey(seed, type, hasResource ? 1u : 0u);
}

}  // namespace ce::streamline_ui_tag_log
