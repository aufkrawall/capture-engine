// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The testable part of ce_enumerate_monitors: argument checks, the id and name rules and the capacity/count rules.
// The entry point (runtime_monitors.cpp) supplies the OS listing; unit tests supply their own.

#include "include/cengine/cengine.h"
#include "common/platform/monitor_selection.h"

#include <functional>
#include <vector>

namespace ce::capi {

using MonitorListing = std::function<std::vector<ce::monitor_selection::MonitorInfo>()>;

// Checks the arguments before the listing runs. Then fills min(capacity, total) items in listing order and reports the
// total in *count. An id that does not fit ce_monitor_info_t::id is skipped (logged), never cut. A name longer than its
// field is cut at a UTF-8 character boundary.
ce_status_t EnumerateMonitors(const MonitorListing& listing, ce_monitor_info_t* items, uint32_t itemSize,
                              uint32_t capacity, uint32_t* count);

}  // namespace ce::capi
