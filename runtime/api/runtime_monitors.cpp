// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// ce_enumerate_monitors: the active displays, as the --list-monitors text lists them (no runtime handle needed). The
// argument and capacity rules live in monitor_list.cpp; this file only supplies the OS listing. Product only.
#include "include/cengine/cengine.h"

#include "common/platform/monitor_selection.h"
#include "runtime/api/monitor_list.h"
#include "runtime/api/runtime_handle.h"

extern "C" ce_status_t ce_enumerate_monitors(ce_monitor_info_t* items, uint32_t item_size, uint32_t capacity,
                                             uint32_t* count) {
    return ce::capi::RunGuarded("ce_enumerate_monitors", [&]() -> ce_status_t {
        return ce::capi::EnumerateMonitors([]() { return ce::monitor_selection::ListMonitors(); }, items, item_size,
                                           capacity, count);
    });
}
