#pragma once

// Shared includes and helpers for the test_fps_limiter suite, which is split
// across several .cpp files to stay under the AGENTS.md size ceiling.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include "../hook/common/fps_limiter.h"
#include "../hook/common/fps_limiter_policy.h"
#include "../hook/common/freeze_watchdog.h"
#include "../hook/common/overlay_compat.h"

class FpsLimiterTest : public ::testing::Test {
protected:
    std::unique_ptr<SharedMemoryLayout> mockShm;
    FpsLimiter limiter;
    LARGE_INTEGER freq;

    void SetUp() override {
        mockShm = std::make_unique<SharedMemoryLayout>();
        limiter.SetSharedMemory(mockShm.get());
        limiter.ResetMissedFrames();
        QueryPerformanceFrequency(&freq);
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetHeuristicFSRFGActive(false);
        g_FGCompat.SetDormantMode(true);  // Reset to default dormant state
    }
};






















namespace {
static NV_SET_SLEEP_MODE_PARAMS g_TestSetSleepModeParams[4]{};
static std::atomic<int> g_TestSetSleepModeCallCount{0};

NvAPI_Status __cdecl TestManualRearmSetSleepMode(IUnknown*, NV_SET_SLEEP_MODE_PARAMS* params) {
    const int index = g_TestSetSleepModeCallCount.fetch_add(1, std::memory_order_acq_rel);
    constexpr int kParamCount =
        static_cast<int>(sizeof(g_TestSetSleepModeParams) / sizeof(g_TestSetSleepModeParams[0]));
    if (index >= 0 && index < kParamCount && params) {
        g_TestSetSleepModeParams[index] = *params;
    }
    return NVAPI_OK;
}

void ResetManualRearmSetSleepModeRecorder() {
    g_TestSetSleepModeCallCount.store(0, std::memory_order_release);
    for (auto& params : g_TestSetSleepModeParams) {
        params = {};
    }
}
}  // namespace







































