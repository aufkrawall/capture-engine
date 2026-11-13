// The final-present VSync report (hook/present/final_present_vsync_report.h): what reached DXGI and at what rate.

#include <gtest/gtest.h>

#include "hook/present/final_present_vsync_report.h"

namespace {

using ce::present_vsync_report::FinalPresentParameters;
using ce::present_vsync_report::FinalPresentVSyncReport;
using ce::present_vsync_report::FinalPresentVSyncRoute;

FinalPresentParameters RuntimeOutput(uint32_t outSync, uint32_t outFlags) {
    FinalPresentParameters parameters;
    parameters.route = FinalPresentVSyncRoute::kRuntimeOutput;
    parameters.inSync = outSync;
    parameters.inFlags = outFlags;
    parameters.outSync = outSync;
    parameters.outFlags = outFlags;
    return parameters;
}

// GTA V Enhanced's menu with FSR FG suspended (session gtafsr): 157 presents/s on a 144 Hz panel. The heartbeat
// must carry the parameters together with the rate they ran at.
TEST(FinalPresentVSyncReportTest, HeartbeatReportsTheRateUnderSteadyParameters) {
    FinalPresentVSyncReport report;
    int64_t nowUs = 1'000'000;
    EXPECT_TRUE(report.Observe(RuntimeOutput(1, 0), nowUs).log);
    FinalPresentVSyncReport::Line heartbeat;
    int presents = 0;
    while (!heartbeat.log) {
        nowUs += 6369;  // 157 Hz
        heartbeat = report.Observe(RuntimeOutput(1, 0), nowUs);
        ++presents;
        ASSERT_LT(presents, 2000);
    }
    EXPECT_TRUE(heartbeat.heartbeat);
    EXPECT_EQ(heartbeat.presents, static_cast<uint64_t>(presents));
    EXPECT_NEAR(heartbeat.presentsPerSecond, 157.0, 0.5);
    EXPECT_EQ(heartbeat.changesSwallowed, 0u);
}

TEST(FinalPresentVSyncReportTest, AParameterChangeLogsAtOnce) {
    FinalPresentVSyncReport report;
    ASSERT_TRUE(report.Observe(RuntimeOutput(1, 0), 100).log);
    EXPECT_FALSE(report.Observe(RuntimeOutput(1, 0), 7'000).log);
    const auto changed = report.Observe(RuntimeOutput(0, 0x200), 14'000);
    EXPECT_TRUE(changed.log);
    EXPECT_FALSE(changed.heartbeat);
    EXPECT_EQ(changed.presents, 2u);

    // The route alone is a change too: the same parameters stated by CE instead of preserved.
    FinalPresentParameters applied = RuntimeOutput(0, 0x200);
    applied.route = FinalPresentVSyncRoute::kApplied;
    EXPECT_TRUE(report.Observe(applied, 21'000).log);
}

// Two swapchains alternating would change the key on every present: the lines stay bounded and the next heartbeat
// counts what it swallowed.
TEST(FinalPresentVSyncReportTest, AlternatingParametersAreBoundedBetweenHeartbeats) {
    FinalPresentVSyncReport report;
    int64_t nowUs = 0;
    uint32_t logged = 0;
    for (int i = 0; i < 1000; ++i) {
        nowUs += 1000;
        if (report.Observe(RuntimeOutput(i % 2, 0), nowUs).log)
            ++logged;
    }
    EXPECT_EQ(logged, FinalPresentVSyncReport::kMaxChangeLinesPerHeartbeat);
    nowUs += FinalPresentVSyncReport::kHeartbeatUs;
    const auto heartbeat = report.Observe(RuntimeOutput(1, 0), nowUs);  // the last key: no change
    ASSERT_TRUE(heartbeat.heartbeat);
    EXPECT_EQ(heartbeat.changesSwallowed, 1000u - FinalPresentVSyncReport::kMaxChangeLinesPerHeartbeat);
    // A fresh heartbeat period logs changes again.
    EXPECT_TRUE(report.Observe(RuntimeOutput(0, 0), nowUs + 1000).log);
}

}  // namespace
