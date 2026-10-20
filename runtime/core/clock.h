// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <cstdint>

namespace ce::engine {

// The runtime's notion of time, injectable so tests never wait: `nowMs` is the tick clock recording
// timestamps and spawn budgets use, `nowUs` the high-resolution clock of the startup/diagnostic lines.
struct Clock {
    uint64_t (*nowMs)() noexcept = nullptr;
    int64_t (*nowUs)() noexcept = nullptr;
};

// GetTickCount64 and Log_GetQpcUs.
Clock SystemClock() noexcept;

}  // namespace ce::engine
