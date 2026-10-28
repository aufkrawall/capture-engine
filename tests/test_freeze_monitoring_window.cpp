#include <gtest/gtest.h>

#include "hook/runtime/freeze_monitoring_window.h"
#include "hook/runtime/freeze_watchdog.h"

namespace {

using ce::freeze_watchdog_policy::MonitoringWindow;
using Transition = MonitoringWindow::Transition;
constexpr uint64_t kSecond = 1'000'000;
constexpr uint64_t kHeartbeat = 10 * kSecond;

TEST(FreezeMonitoringWindowTest, ZeroClockOriginIsAValidMonitoringEpoch) {
    MonitoringWindow window;
    EXPECT_EQ(window.ElapsedMicros(kSecond, 0), 0u);
    EXPECT_EQ(window.Observe(0, true), Transition::Resumed);
    EXPECT_EQ(window.ElapsedMicros(kSecond, 0), kSecond);
    EXPECT_EQ(window.Observe(kSecond, false), Transition::Suspended);
    window.Reset(0);
    EXPECT_EQ(window.ElapsedMicros(kSecond, 0), kSecond);
}

TEST(FreezeMonitoringWindowTest, BackgroundPauseDoesNotConsumeForegroundFreezeTimeout) {
    MonitoringWindow window;
    window.Reset(kHeartbeat);
    EXPECT_EQ(window.Observe(kHeartbeat, true), Transition::None);
    EXPECT_EQ(window.Observe(kHeartbeat + kSecond, false), Transition::Suspended);
    EXPECT_EQ(window.Observe(kHeartbeat + 90 * kSecond, false), Transition::None);
    EXPECT_EQ(window.ElapsedMicros(kHeartbeat + 90 * kSecond, kHeartbeat), 0u);

    // Metro returned from a 91-second Alt+Tab pause and recreated its swapchain.
    // No new Present yet: that pause must not be reported as a render-thread freeze.
    constexpr uint64_t resumed = kHeartbeat + 91 * kSecond;
    EXPECT_EQ(window.Observe(resumed, true), Transition::Resumed);
    EXPECT_EQ(window.ElapsedMicros(resumed, kHeartbeat), 0u);
    EXPECT_EQ(window.Observe(resumed + 29 * kSecond, true), Transition::None);
    EXPECT_EQ(window.ElapsedMicros(resumed + 29 * kSecond, kHeartbeat), 29 * kSecond);
    // A real failure to resume presentation still reaches the ordinary timeout.
    EXPECT_GT(window.ElapsedMicros(resumed + 31 * kSecond, kHeartbeat), 30 * kSecond);
}

TEST(FreezeMonitoringWindowTest, FreshPresentOwnsTheDeadlineAfterResume) {
    MonitoringWindow window;
    window.Reset(kHeartbeat);
    window.Observe(kHeartbeat + kSecond, false);
    constexpr uint64_t resumed = kHeartbeat + 91 * kSecond;
    window.Observe(resumed, true);
    EXPECT_EQ(window.ElapsedMicros(resumed + 4 * kSecond, resumed + 3 * kSecond), kSecond);
    EXPECT_EQ(window.ElapsedMicros(resumed, resumed + 1), 0u);
    window.Reset(resumed + 10 * kSecond);
    EXPECT_EQ(window.ElapsedMicros(resumed + 11 * kSecond, resumed + 3 * kSecond), kSecond);
}

TEST(FreezeMonitoringWindowTest, RepeatedBackgroundEpisodesEachStartANewMonitoringWindow) {
    MonitoringWindow window;
    window.Reset(kHeartbeat);
    for (uint64_t episode = 1; episode <= 3; ++episode) {
        const uint64_t background = kHeartbeat + episode * 100 * kSecond;
        EXPECT_EQ(window.Observe(background, false), Transition::Suspended);
        EXPECT_EQ(window.Observe(background + 90 * kSecond, true), Transition::Resumed);
        EXPECT_EQ(window.ElapsedMicros(background + 91 * kSecond, kHeartbeat), kSecond);
    }
}

TEST(FreezeMonitoringWindowTest, InFlightPresentAndForcedMonitoringKeepTheirDeadlineInBackground) {
    for (int reason = 0; reason < 3; ++reason) {
        SCOPED_TRACE(reason);
        MonitoringWindow window;
        window.Reset(kHeartbeat);
        const bool suppressed = ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(
            false, reason == 0, reason == 1, reason == 2);
        ASSERT_FALSE(suppressed);
        EXPECT_EQ(window.Observe(kHeartbeat + 31 * kSecond, !suppressed), Transition::None);
        EXPECT_GT(window.ElapsedMicros(kHeartbeat + 31 * kSecond, kHeartbeat), 30 * kSecond);
    }
}

}  // namespace
