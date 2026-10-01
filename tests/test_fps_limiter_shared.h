#pragma once

// Shared includes and helpers for the test_fps_limiter suite, which is split
// across several .cpp files to stay under the AGENTS.md size ceiling.

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include "../hook/common/fps_limiter.h"
#include "../hook/common/fps_limiter_policy.h"
#include "../hook/common/freeze_watchdog.h"
#include "../hook/common/overlay_compat.h"

// Call-site contracts under test. See ce::fps_limiter_policy::PresentSite.
inline constexpr ce::fps_limiter_policy::PresentSite kFinalOutputSite =
    ce::fps_limiter_policy::PresentSite::kFinalOutputBoundary;
inline constexpr ce::fps_limiter_policy::PresentSite kUniquePresentSite =
    ce::fps_limiter_policy::PresentSite::kUniqueApplicationPresent;
inline constexpr ce::fps_limiter_policy::PresentSite kDuplicateProneSite =
    ce::fps_limiter_policy::PresentSite::kDuplicateProne;

// A QPC-tick clock that moves only when the limiter waits on it or the test
// says time passed. See FpsLimiter::SetClockSourceForTesting for why a test
// that asserts WHEN the limiter releases a caller must not use the real one.
class VirtualLimiterClock {
public:
    int64_t Now() const {
        return now_.load(std::memory_order_relaxed);
    }
    // The game working, a hitch, or any other time the limiter does not own.
    void Advance(int64_t ticks) {
        now_.fetch_add(ticks, std::memory_order_relaxed);
    }
    FpsLimiter::ClockSource Source() {
        FpsLimiter::ClockSource source;
        source.context = this;
        source.now = [](void* context) { return static_cast<VirtualLimiterClock*>(context)->Now(); };
        source.waitUntil = [](void* context, int64_t targetTick) {
            auto* clock = static_cast<VirtualLimiterClock*>(context);
            if (targetTick > clock->Now()) {
                clock->now_.store(targetTick, std::memory_order_relaxed);
            }
        };
        return source;
    }

private:
    // Far from zero: the limiter uses 0 as "no deadline armed yet".
    std::atomic<int64_t> now_{int64_t{1} << 40};
};

class FpsLimiterTest : public ::testing::Test {
protected:
    std::unique_ptr<SharedMemoryLayout> mockShm;
    FpsLimiter limiter;
    LARGE_INTEGER freq;
    VirtualLimiterClock clock;

    // Opt in per test, before the first Apply(). The SmartWait tests keep the
    // real clock on purpose: the real wait primitive is what they examine.
    void UseVirtualClock() {
        limiter.SetClockSourceForTesting(clock.Source());
    }
    // Virtual ticks one call to `fn` took - exactly the wait the limiter chose.
    template <typename Fn>
    int64_t VirtualTicksOf(Fn&& fn) {
        const int64_t start = clock.Now();
        fn();
        return clock.Now() - start;
    }
    int64_t TicksFromUs(int64_t us) const {
        return us * freq.QuadPart / 1000000;
    }
    // The first deadline a fresh local cadence arms: half a (scaled) interval.
    int64_t FirstSlotTicks(int targetFps, int cadenceScale = 1) const {
        return std::max<int64_t>(1, (freq.QuadPart / targetFps) * cadenceScale / 2);
    }
    // The first full group interval after that slot (rational remainder 0).
    int64_t FirstIntervalTicks(int targetFps, int cadenceScale = 1) const {
        int64_t remainder = 0;
        return ce::fps_limiter_policy::NextRationalGroupIntervalTicks(freq.QuadPart, targetFps, cadenceScale,
                                                                      remainder);
    }

    void SetUp() override {
        // Every test, virtual limiter clock or not: Reflex recency ("did the
        // game sleep within 500 ms") is judged on the virtual clock, which
        // stands still unless the limiter waits or the test advances it. A
        // host load spike can therefore never age a sleep the test just marked.
        ReflexLimiter::TickSource ticks;
        ticks.context = &clock;
        ticks.nowMs = [](void* context) {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return static_cast<ULONGLONG>(static_cast<VirtualLimiterClock*>(context)->Now() * 1000 /
                                          frequency.QuadPart);
        };
        g_ReflexLimiter.SetTickSourceForTesting(ticks);
        mockShm = std::make_unique<SharedMemoryLayout>();
        limiter.SetSharedMemory(mockShm.get());
        limiter.ResetMissedFrames();
        QueryPerformanceFrequency(&freq);
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetHeuristicFSRFGActive(false);
        g_ReflexLimiter.SetGameActivated(false);
        g_FGCompat.SetDormantMode(true);  // Reset to default dormant state
    }

    void ConfirmDLSSFGPacing() {
        g_ReflexLimiter.SetGameActivated(true);
        g_ReflexLimiter.MarkGameSleep("unit-test");
    }

    void TearDown() override {
        g_ReflexLimiter.SetGameActivated(false);
        // g_ReflexLimiter outlives this fixture and its clock.
        g_ReflexLimiter.SetTickSourceForTesting({});
    }
};
