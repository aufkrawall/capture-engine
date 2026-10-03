#pragma once

// Metering of the "UI tag record opportunity" diagnostics (streamline_hook_api.cpp).

#include <cstdint>

#include "common/logging/log_meter.h"

namespace ce::streamline_ui_tag_log {

// The log stream of one tag call: its API, feature and viewport, and its shape. A game can tag several resource
// sets per frame on one viewport (GTA V Enhanced: 3, 4 and 6 tags in turn); a stream without the shape saw every
// alternation as a change and logged every call (25k lines, 47% of hook_debug.log in session 20261003_120641).
inline uint64_t Stream(const char* api, uint32_t feature, uint32_t viewport, uint32_t numTags, uint32_t numInputs) {
    return ce::log_meter::FieldKey(api, feature, viewport, numTags, numInputs);
}

}  // namespace ce::streamline_ui_tag_log
