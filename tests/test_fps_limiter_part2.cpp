#include "test_fps_limiter_shared.h"

namespace {

struct MockNativePacingBackend {
    bool available = true;
    bool gameActive = false;
    bool setTargetSucceeds = true;
    int targetFps = 0;
    int setTargetCalls = 0;
    int sleepCalls = 0;
    int clearCalls = 0;
};

NativeFpsPacingBackend MakeMockNativePacingBackend(MockNativePacingBackend* mock) {
    NativeFpsPacingBackend backend{};
    backend.context = mock;
    backend.isAvailable = [](void* context) { return static_cast<MockNativePacingBackend*>(context)->available; };
    backend.isGameActive = [](void* context) { return static_cast<MockNativePacingBackend*>(context)->gameActive; };
    backend.setTargetFps = [](void* context, int fps) {
        auto* state = static_cast<MockNativePacingBackend*>(context);
        ++state->setTargetCalls;
        state->targetFps = fps;
        return state->available && state->setTargetSucceeds;
    };
    backend.sleep = [](void* context, int64_t* waitUs) {
        auto* state = static_cast<MockNativePacingBackend*>(context);
        ++state->sleepCalls;
        *waitUs = 321;
        return state->available;
    };
    backend.clear = [](void* context) { ++static_cast<MockNativePacingBackend*>(context)->clearCalls; };
    backend.name = "test native backend";
    return backend;
}

}  // namespace

TEST(FpsLimiterPolicyTest, EveryGeneralLimiterModeTargetsFinalFrameGeneratedRate) {
    using ce::fps_limiter_policy::ResolveFrameGenerationBaseTarget;

    EXPECT_EQ(ResolveFrameGenerationBaseTarget(100, true, 3, true), 33);
    EXPECT_EQ(ResolveFrameGenerationBaseTarget(120, true, 4, true), 30);
    EXPECT_EQ(ResolveFrameGenerationBaseTarget(100, true, 2, true), 50);
    EXPECT_EQ(ResolveFrameGenerationBaseTarget(100, false, 3, true), 100);
    EXPECT_EQ(ResolveFrameGenerationBaseTarget(100, true, 3, false), 100)
        << "ordinary/base inject capture-sync targets application frames, unlike the general output cap";
}

TEST(FpsLimiterPolicyTest, GeneralCapRemainsAConcurrentConstraintDuringBaseInjectCapture) {
    using ce::fps_limiter_policy::LimiterConstraintSource;
    using ce::fps_limiter_policy::ResolveLimiterTargetSelection;

    for (int effectiveMultiplier = 2; effectiveMultiplier <= 4; ++effectiveMultiplier) {
        const auto selected = ResolveLimiterTargetSelection(
            true, true, 120, 1, false, true, 120, true, effectiveMultiplier,
            true, false);
        EXPECT_EQ(selected.source, LimiterConstraintSource::kGeneral)
            << "effective game/override multiplier " << effectiveMultiplier;
        EXPECT_EQ(selected.targetFps, 120);
        EXPECT_EQ(selected.captureOutputEquivalentFps, 120 * effectiveMultiplier);
        EXPECT_FALSE(selected.captureSourceIsFinalOutput);
    }

    const auto fgSuspended = ResolveLimiterTargetSelection(
        true, true, 120, 1, false, true, 120, false, 1, true, false);
    EXPECT_EQ(fgSuspended.source, LimiterConstraintSource::kCaptureSync);
    EXPECT_EQ(fgSuspended.captureOutputEquivalentFps, 120);
}

TEST(FpsLimiterPolicyTest, FinalOutputInjectRouteKeepsCaptureGridAtDisplayedRate) {
    using ce::fps_limiter_policy::LimiterConstraintSource;
    using ce::fps_limiter_policy::ResolveLimiterTargetSelection;

    const auto finalOutput = ResolveLimiterTargetSelection(
        true, true, 120, 1, false, true, 120, true, 4, true, true);
    EXPECT_EQ(finalOutput.source, LimiterConstraintSource::kCaptureSync);
    EXPECT_EQ(finalOutput.targetFps, 120);
    EXPECT_EQ(finalOutput.captureOutputEquivalentFps, 120);
    EXPECT_TRUE(finalOutput.captureSourceIsFinalOutput);

    const auto looserGeneral = ResolveLimiterTargetSelection(
        true, true, 60, 1, false, true, 240, true, 3, true, false);
    EXPECT_EQ(looserGeneral.source, LimiterConstraintSource::kCaptureSync);
    EXPECT_EQ(looserGeneral.captureOutputEquivalentFps, 180);

    const auto vfr = ResolveLimiterTargetSelection(
        true, true, 60, 1, true, true, 120, true, 3, true, false);
    EXPECT_EQ(vfr.source, LimiterConstraintSource::kGeneral);
}

TEST_F(FpsLimiterTest, ReflexGeneralCapSurvivesCaptureAndEffectiveMfgFactorChanges) {
    MockNativePacingBackend mock;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(120);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));

    for (int effectiveMultiplier : {2, 3, 4}) {
        g_FGCompat.SetDLSSFGMultiplier(effectiveMultiplier);
        g_FGCompat.SetDLSSFGActive(true);
        limiter.Apply(true);
        EXPECT_EQ(mock.targetFps, 120)
            << "game-selected and overridden factors share the published effective multiplier";
        limiter.CancelPostPresentPacing();
    }

    g_FGCompat.SetDLSSFGActive(false);
    limiter.Apply(true);
    EXPECT_EQ(mock.targetFps, 120) << "temporary FG suspension keeps the same displayed-rate contract";
    limiter.CancelPostPresentPacing();
    limiter.Shutdown();
}

TEST_F(FpsLimiterTest, ReflexFinalOutputInjectRouteUsesDisplayedCaptureTarget) {
    MockNativePacingBackend mock;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    limiter.SetInjectFinalOutputCaptureAvailable(true);
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(true);

    EXPECT_EQ(mock.targetFps, 60) << "the final-output route must not be multiplied to 180";
    limiter.CancelPostPresentPacing();
    limiter.Shutdown();
}

// Basic mode has the same final-output target semantics as native/FG-fallback
// mode. Capture sync avoids the intentional immediate duplicate-present guard,
// making the scaled interval directly measurable without a timing sleep.
TEST_F(FpsLimiterTest, BasicCaptureSyncScalesToFinalFGOutputRate) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply();

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);
    const double elapsedMs =
        static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);

    EXPECT_GE(elapsedMs, 25.0);
    EXPECT_LT(elapsedMs, 100.0);
    g_FGCompat.SetDLSSFGActive(false);
}

TEST_F(FpsLimiterTest, ExplicitNativeModeUsesApiBackendAfterPresent) {
    MockNativePacingBackend mock;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(100);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));

    limiter.Apply(true);

    EXPECT_EQ(mock.setTargetCalls, 1);
    EXPECT_EQ(mock.targetFps, 100);
    EXPECT_EQ(mock.sleepCalls, 0);
    EXPECT_FALSE(limiter.IsActivelyLimiting());

    limiter.ApplyPostPresent();
    EXPECT_EQ(mock.sleepCalls, 1);
    EXPECT_EQ(limiter.GetLastWaitUs(), 321);
    limiter.Shutdown();
}

// The Vulkan low-latency interval belongs to the NVIDIA driver, which stretches
// the render loop by the DLSS-G/MFG factor itself, so it takes the configured
// OUTPUT rate. Handing it the FG-divided base target capped Portal RTX at
// target/multiplier displayed fps.
TEST_F(FpsLimiterTest, VulkanNativeTargetStaysTheOutputRateForMfg) {
    MockNativePacingBackend mock;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(90);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(true);

    EXPECT_EQ(mock.targetFps, 90);
    limiter.CancelPostPresentPacing();
    g_FGCompat.SetDLSSFGActive(false);
    limiter.Shutdown();
}

TEST_F(FpsLimiterTest, AutoModeUsesAnAlreadyActiveVulkanNativeBackend) {
    MockNativePacingBackend mock;
    mock.gameActive = true;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(100);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));

    limiter.Apply(true);

    EXPECT_EQ(mock.setTargetCalls, 1);
    EXPECT_EQ(mock.targetFps, 100);
    limiter.CancelPostPresentPacing();
    limiter.Shutdown();
}

TEST_F(FpsLimiterTest, NativeRetargetFailureClearsPersistentDriverIntervalBeforeFallback) {
    MockNativePacingBackend mock;
    mock.setTargetSucceeds = false;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(100);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));

    limiter.Apply(true);

    EXPECT_EQ(mock.setTargetCalls, 1);
    EXPECT_EQ(mock.clearCalls, 1);
    EXPECT_EQ(mock.sleepCalls, 0);
    EXPECT_TRUE(limiter.IsActivelyLimiting());
    limiter.Shutdown();
}

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
