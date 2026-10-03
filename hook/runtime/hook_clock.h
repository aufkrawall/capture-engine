#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>

// The clock behind every hook-side time reading: GetTickCount64/GetTickCount, QueryPerformanceCounter and
// steady_clock. In a game it is the system clock, read directly (one well-predicted branch). The FG flow
// tests (tests/flow) switch it to a virtual clock that their game advances by one frame interval per frame,
// so CE's time windows - startup windows, cooldowns, cadence and stall checks - span the same frames as in a
// real game however fast the WARP test game renders. QueryPerformanceFrequency stays real; the virtual counter
// counts in the same units.
namespace ce::hook_clock {

struct VirtualClock {
    std::atomic<bool> enabled{false};
    std::atomic<int64_t> counter{0};  // QueryPerformanceCounter units
    std::atomic<int64_t> frequency{1};
};

inline VirtualClock& Virtual() {
    static VirtualClock clock;
    return clock;
}

inline BOOL QueryCounter(LARGE_INTEGER* counter) {
    VirtualClock& clock = Virtual();
    if (clock.enabled.load(std::memory_order_acquire)) {
        counter->QuadPart = clock.counter.load(std::memory_order_acquire);
        return TRUE;
    }
    return QueryPerformanceCounter(counter);
}

inline ULONGLONG TickCount64() {
    VirtualClock& clock = Virtual();
    if (clock.enabled.load(std::memory_order_acquire)) {
        const int64_t perMillisecond = clock.frequency.load(std::memory_order_relaxed) / 1000;
        return static_cast<ULONGLONG>(clock.counter.load(std::memory_order_acquire) / perMillisecond);
    }
    return GetTickCount64();
}

inline DWORD TickCount() {
    return static_cast<DWORD>(TickCount64());
}

inline std::chrono::steady_clock::time_point SteadyNow() {
    VirtualClock& clock = Virtual();
    if (clock.enabled.load(std::memory_order_acquire)) {
        const int64_t counter = clock.counter.load(std::memory_order_acquire);
        const int64_t frequency = clock.frequency.load(std::memory_order_relaxed);
        const auto seconds = counter / frequency;
        const auto remainder = counter % frequency;
        const auto nanoseconds =
            std::chrono::nanoseconds(seconds * 1000000000ll + remainder * 1000000000ll / frequency);
        return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(nanoseconds));
    }
    return std::chrono::steady_clock::now();
}

// Test host only: switches to the virtual clock, starting at the current real counter value so readings stay
// plausible; then only Advance* moves it.
inline void EnableVirtual() {
    VirtualClock& clock = Virtual();
    LARGE_INTEGER frequency{};
    LARGE_INTEGER now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    clock.frequency.store(frequency.QuadPart, std::memory_order_relaxed);
    clock.counter.store(now.QuadPart, std::memory_order_release);
    clock.enabled.store(true, std::memory_order_release);
}

inline void AdvanceMicroseconds(int64_t microseconds) {
    VirtualClock& clock = Virtual();
    clock.counter.fetch_add(microseconds * clock.frequency.load(std::memory_order_relaxed) / 1000000,
                            std::memory_order_acq_rel);
}

}  // namespace ce::hook_clock
