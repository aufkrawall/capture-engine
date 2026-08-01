#include "test_fps_limiter_shared.h"

TEST(OverlayCompatTest, SameProcessForegroundWindowCanDrivePostResumeCountdown) {
    LONG width = 0;
    LONG height = 0;
    bool usingSameProcessForegroundWindow = false;

    EXPECT_TRUE(ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        false, true, 320, 200, 1280, 720, &width, &height, &usingSameProcessForegroundWindow));
    EXPECT_TRUE(usingSameProcessForegroundWindow);
    EXPECT_EQ(1280, width);
    EXPECT_EQ(720, height);
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, false, true, width,
                                                                             height, 0, 100));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, false, true, width,
                                                                              height, 100, 100));
}

TEST(OverlayCompatTest, InvalidForegroundWindowDoesNotCreatePostResumeCandidate) {
    LONG width = 0;
    LONG height = 0;
    bool usingSameProcessForegroundWindow = false;

    // When the foreground window is not usable but the game window itself is,
    // we fall back to tracking the game window (e.g. controller pseudo-overlay
    // stole focus while the game is still actively rendering).
    EXPECT_TRUE(ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        false, false, 1280, 720, 1280, 720, &width, &height, &usingSameProcessForegroundWindow));
    EXPECT_FALSE(usingSameProcessForegroundWindow);
    EXPECT_EQ(1280, width);
    EXPECT_EQ(720, height);

    // If the same-process foreground window is explicitly offered but is too
    // small, the candidate is rejected.
    EXPECT_FALSE(ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        false, true, 1280, 720, 320, 200, &width, &height, &usingSameProcessForegroundWindow));
    EXPECT_FALSE(usingSameProcessForegroundWindow);

    // If neither the game window nor the foreground window is usable, there is
    // no candidate.
    EXPECT_FALSE(ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        false, false, 320, 200, 320, 200, &width, &height, &usingSameProcessForegroundWindow));
    EXPECT_FALSE(usingSameProcessForegroundWindow);
}

TEST(OverlayCompatTest, SwapchainWindowFallbackWhenForegroundIsExternal) {
    LONG width = 0;
    LONG height = 0;
    bool usingSameProcessForegroundWindow = false;

    // Controller pseudo-overlay (external process, zero-sized) is foreground.
    // Game window is valid and should be tracked.
    EXPECT_TRUE(ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        false, false, 3840, 2160, 0, 0, &width, &height, &usingSameProcessForegroundWindow));
    EXPECT_FALSE(usingSameProcessForegroundWindow);
    EXPECT_EQ(3840, width);
    EXPECT_EQ(2160, height);

    // The delay function should still work with the fallback dimensions.
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, false, true, width,
                                                                             height, 0, 100));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, false, true, width,
                                                                              height, 100, 100));
}

TEST(OverlayCompatTest, StartupCompatibleAllocatorPoolCanShrinkForStartupOverlay) {
    EXPECT_EQ(3u, ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(true, true, false, false, 16));
    EXPECT_EQ(16u, ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(true, false, false, false, 16));
    EXPECT_EQ(16u, ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(true, true, true, false, 16));
    EXPECT_EQ(16u, ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(false, true, false, false, 16));
    EXPECT_EQ(16u, ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(true, true, false, true, 16));
}

TEST(OverlayCompatTest, StartupCompatibleRenderDelayOnlyAppliesBeforeSettle) {
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(true, false, 4999, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(true, false, 5000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(true, true, 1000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(false, false, 1000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(true, false, 0, 5000, true));
}

TEST(OverlayCompatTest, StartupBlockingOverlayCanSuppressDX12Render) {
    EXPECT_TRUE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
        true, false, "SocialClubD3D12Renderer.dll", 1000, 30000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
        true, true, "SocialClubD3D12Renderer.dll", 1000, 30000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
        true, false, "SocialClubD3D12Renderer.dll", 30000, 30000));
    EXPECT_FALSE(
        ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(true, false, nullptr, 1000, 30000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
        false, false, "SocialClubD3D12Renderer.dll", 1000, 30000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
        true, false, "SocialClubD3D12Renderer.dll", 1000, 30000, true));
}

TEST(OverlayCompatTest, RecentBlockingRendererActivityExtendsDX12RenderSuppression) {
    EXPECT_TRUE(ce::overlay_compat::HasRecentDX12StartupBlockingRenderActivity(9000, 10000, 2000));
    EXPECT_FALSE(ce::overlay_compat::HasRecentDX12StartupBlockingRenderActivity(8000, 10000, 2000));
    EXPECT_FALSE(ce::overlay_compat::HasRecentDX12StartupBlockingRenderActivity(0, 10000, 2000));
    EXPECT_TRUE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
        true, false, "SocialClubD3D12Renderer.dll", true));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
        true, false, "SocialClubD3D12Renderer.dll", false));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
        true, true, "SocialClubD3D12Renderer.dll", true));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
        true, false, "SocialClubD3D12Renderer.dll", true, true));
}

TEST(OverlayCompatTest, InitializedOverlayStaysVisibleDuringStartupSuppression) {
    EXPECT_TRUE(ce::overlay_compat::ShouldKeepDX12OverlayVisibleDuringStartupSuppression(true));
    EXPECT_FALSE(ce::overlay_compat::ShouldKeepDX12OverlayVisibleDuringStartupSuppression(false));
}

TEST(OverlayCompatTest, DedicatedQueueIsFGOnly) {
    // FG uses the dedicated overlay queue.
    EXPECT_TRUE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(true, false, nullptr));
    EXPECT_TRUE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(true, false, "sl.interposer.dll"));

    // Non-FG stays single-queue: a non-owning queue drawing directly to the swapchain backbuffer
    // is rejected by DXGI with DXGI_ERROR_ACCESS_DENIED (0x887A002B) and removes the device on
    // the first submit (logs/20260606_153428). Only the swapchain's owning (app) queue may render
    // the backbuffer, so plain non-FG must stay single-queue.
    EXPECT_FALSE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(false, false, nullptr));
    EXPECT_FALSE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(false, true, nullptr));
    EXPECT_FALSE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(false, false, "socialclub.dll"));
}

TEST_F(FpsLimiterTest, FreezeWatchdogTimeoutOnlyExpandsForActiveDLSSFG) {
    FreezeWatchdog watchdog;
    const double baselineTimeout = watchdog.GetRecommendedTimeout();

    g_FGCompat.SetDLSSFGActive(true);
    EXPECT_GT(watchdog.GetRecommendedTimeout(), baselineTimeout);

    g_FGCompat.SetDLSSFGActive(false);
    EXPECT_DOUBLE_EQ(watchdog.GetRecommendedTimeout(), baselineTimeout);
}

TEST_F(FpsLimiterTest, FreezeWatchdogDefersSelfTargetedImmediateDumpCapture) {
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldDeferImmediateDumpToWatchdogThread(42, 42));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldDeferImmediateDumpToWatchdogThread(42, 7));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldDeferImmediateDumpToWatchdogThread(42, 0));
}

TEST_F(FpsLimiterTest, FreezeWatchdogDumpCaptureIsOneShotPerRun) {
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldCaptureWatchdogDump(false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldCaptureWatchdogDump(true));
}

TEST_F(FpsLimiterTest, FreezeWatchdogKeepsRuntimePresentationMonitoredInBackground) {
    EXPECT_TRUE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, false, false, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, false, false, true));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, false, true, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(false, true, false, false));
    EXPECT_FALSE(ce::freeze_watchdog_policy::ShouldSuppressFreezeCheckForBackgroundProcess(true, false, false, false));
}
