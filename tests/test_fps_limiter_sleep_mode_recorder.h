#pragma once

#include <atomic>

#include "hook/pacing/reflex_limiter.h"

// Only the manual sleep-mode rearm suites need this per-translation-unit recorder.
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
