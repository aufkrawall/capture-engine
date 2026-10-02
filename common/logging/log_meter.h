#pragma once

#include <cstdint>

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

namespace ce::log_meter {

// Metering helper for diagnostic logging on hot or frequently repeated paths.
//
// `callIndex` is 1-based; callers keep a static counter (std::atomic<int> on
// multi-threaded paths) and pass the next value. Returns true for the first
// `firstBurstCount` calls and then for every `stride`-th call (callIndex ==
// stride, 2*stride, ...). A stride of 0 logs every call.
//
// This keeps the "first N entries plus a periodic heartbeat" policy used across
// the codebase in one testable place instead of a new magic-number condition at
// every call site.
inline bool ShouldLogCadence(uint32_t callIndex, uint32_t firstBurstCount, uint32_t stride) {
    if (stride == 0) {
        return true;
    }
    return callIndex <= firstBurstCount || (callIndex % stride) == 0;
}

}  // namespace ce::log_meter
