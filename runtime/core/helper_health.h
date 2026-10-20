// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The CE_EVENT_HELPER transitions (llm-wiki/library/03 "Events"), as a pure step function the engine applies
// once per tick. A helper is announced READY the first time it is ready, LOST when it disappears while it is
// still wanted, and RECOVERED when it returns. A helper that is no longer wanted ends its episode silently: a
// media child retired after every recording and a logger or sensor switched off by settings are not losses.

#include "include/cengine/cengine_draft.h"

#include <cstdint>

namespace ce::engine {

enum class HelperHealth : uint8_t {
    kDown,  // not ready, and no ready episode is open (never ready, or intentionally absent)
    kUp,    // ready; an episode is open
    kLost,  // a ready helper disappeared while it was wanted; waiting for it to return
};

struct HelperStep {
    HelperHealth health = HelperHealth::kDown;
    int32_t event = 0;  // CE_HELPER_STATE_* to announce, or 0 for no event
};

inline HelperStep StepHelper(HelperHealth health, bool ready, bool wanted) {
    if (ready) {
        if (health == HelperHealth::kUp)
            return {HelperHealth::kUp, 0};
        return {HelperHealth::kUp, health == HelperHealth::kLost ? CE_HELPER_STATE_RECOVERED : CE_HELPER_STATE_READY};
    }
    if (!wanted)
        return {HelperHealth::kDown, 0};
    if (health == HelperHealth::kUp)
        return {HelperHealth::kLost, CE_HELPER_STATE_LOST};
    return {health, 0};  // still lost, or still starting: one LOST per episode
}

}  // namespace ce::engine
