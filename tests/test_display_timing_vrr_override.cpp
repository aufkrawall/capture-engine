// Windows' VRR sync-interval override (captureengine/display_timing/display_timing_vrr_override.h): which followed
// processes DXGI replaced SyncInterval=1 for, said once per process.

#include <gtest/gtest.h>

#include <algorithm>

#include "captureengine/display_timing/display_timing_vrr_override.h"
#include "common/platform/display_timing_etw.h"

namespace {

using display_timing_vrr_override::Monitor;

// dxgi!EventOverrideSyncInterval's descriptor in Windows build 26300's dxgi.dll: id 0x4A, task 2 (SwapChain), level 0,
// keyword 0x8000000000000002 - the keyword the runtime provider is already enabled with, so the session only has to
// let the id through its filter.
TEST(DisplayTimingVrrOverrideTest, TheSessionTakesDxgisOverrideEvent) {
    EXPECT_EQ(display_timing_etw::kRuntimeOverrideSyncInterval, 0x4a);
    EXPECT_EQ(display_timing_etw::kRuntimeKeyword & 0x8000000000000002ull, 0x8000000000000002ull);
    const auto& ids = display_timing_etw::kRuntimeEventIds;
    EXPECT_NE(std::find(ids.begin(), ids.end(), display_timing_etw::kRuntimeOverrideSyncInterval), ids.end());
    // The present events stay in the filter next to it.
    EXPECT_NE(std::find(ids.begin(), ids.end(), display_timing_etw::kRuntimePresentStart), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), display_timing_etw::kRuntimeMpoPresentStart), ids.end());
}

// GTA V Enhanced (session 20261113_185020): every present of the process carried the override. One line, every
// override counted.
TEST(DisplayTimingVrrOverrideTest, AProcessIsReportedOnceAndEveryOverrideIsCounted) {
    Monitor monitor;
    EXPECT_TRUE(monitor.Observe(28620));
    for (int i = 0; i < 3684; ++i)
        EXPECT_FALSE(monitor.Observe(28620));
    EXPECT_EQ(monitor.overrides(), 3685u);
    EXPECT_EQ(monitor.processes(), 1u);

    // A second followed process (a relaunch) is its own line.
    EXPECT_TRUE(monitor.Observe(23180));
    EXPECT_FALSE(monitor.Observe(28620));
    EXPECT_EQ(monitor.processes(), 2u);
}

// The memory is bounded: past the capacity the oldest process is forgotten and would be reported again, a newer one
// is never swallowed.
TEST(DisplayTimingVrrOverrideTest, PastTheCapacityTheOldestProcessIsForgotten) {
    Monitor monitor;
    for (uint32_t pid = 1; pid <= Monitor::kProcessCapacity; ++pid)
        EXPECT_TRUE(monitor.Observe(pid));
    EXPECT_TRUE(monitor.Observe(1000));  // evicts pid 1
    EXPECT_FALSE(monitor.Observe(2));
    EXPECT_FALSE(monitor.Observe(1000));
    EXPECT_TRUE(monitor.Observe(1));
    EXPECT_EQ(monitor.processes(), Monitor::kProcessCapacity + 2);
}

}  // namespace
