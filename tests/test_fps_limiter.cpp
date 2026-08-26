#include "test_fps_limiter_shared.h"

#include <algorithm>
#include <vector>

// Test the high-precision wait logic
TEST_F(FpsLimiterTest, SmartWait_Accuracy) {
    // Target 16ms from now (approx 60 FPS). A single sample can be delayed by
    // scheduler contention on a fully loaded machine, so assert on the median
    // of several waits while still rejecting early returns and pathological
    // starvation.
    std::vector<double> samples;
    samples.reserve(7);
    for (int i = 0; i < 7; ++i) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);

        const int64_t targetUs = 16666;  // 16.666 ms
        const int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

        // This should block until targetTicks
        ASSERT_TRUE(limiter.SmartWait(targetTicks));

        QueryPerformanceCounter(&end);
        const int64_t elapsedTicks = end.QuadPart - start.QuadPart;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        samples.push_back(static_cast<double>(elapsedTicks) * 1000.0 / freq.QuadPart);
    }

    std::sort(samples.begin(), samples.end());
    const double medianMs = samples[samples.size() / 2];

    // Should never return early.
    EXPECT_GE(samples.front(), 16.0);
    // Median should stay near 16.66ms; allow scheduler jitter on loaded hosts.
    EXPECT_GE(medianMs, 16.0);
    EXPECT_LT(medianMs, 20.0);
    // Even the worst sample must not indicate a broken wait path.
    EXPECT_LT(samples.back(), 60.0);
}

// Test what happens if we are already late
TEST_F(FpsLimiterTest, SmartWait_Late) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Target was 1ms ago
    int64_t targetTicks = now.QuadPart - (freq.QuadPart / 1000);

    // Should return false immediately
    bool waited = limiter.SmartWait(targetTicks);

    EXPECT_FALSE(waited);
}

// Test the SmartWait function directly
TEST_F(FpsLimiterTest, SmartWait_WithTarget) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    // Target 5ms in the future (enough to verify wait, fast enough for tests)
    int64_t targetTicks = start.QuadPart + (5 * freq.QuadPart / 1000);
    bool waited = limiter.SmartWait(targetTicks);

    QueryPerformanceCounter(&end);

    EXPECT_TRUE(waited);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    EXPECT_GE(elapsedMs, 3.0);   // Should wait at least ~3ms
    EXPECT_LT(elapsedMs, 100.0);  // Loaded-host sanity bound; accuracy is covered by the lower bound
}

TEST_F(FpsLimiterTest, Apply_GeneralBasicUsesLocalCadence) {
    // Setup for general FPS limit
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // First local-cadence frame starts around half an interval ahead.
    EXPECT_GE(elapsedMs, 3.0);
    EXPECT_LT(elapsedMs, 100.0);
}

TEST_F(FpsLimiterTest, Apply_NoExternalTargetUsesLocalCadence) {
    // Setup for general FPS limit
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Local cadence should not pay any helper-process event timeout.
    EXPECT_LT(elapsedMs, 100.0);
}

TEST_F(FpsLimiterTest, GeneralBasicUsesLocalCadenceWithoutLimiterProcessTimeout) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(140);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    wchar_t releaseName[64] = {};
    wchar_t requestName[64] = {};
    swprintf(releaseName, 64, L"Local\\CE_TEST_LR_%lu_%lu", GetCurrentProcessId(), GetTickCount());
    swprintf(requestName, 64, L"Local\\CE_TEST_LQ_%lu_%lu", GetCurrentProcessId(), GetTickCount());
    wcscpy_s(mockShm->fpsLimiter.releaseEventName, releaseName);
    wcscpy_s(mockShm->fpsLimiter.requestEventName, requestName);

    HANDLE releaseEvent = CreateEventW(nullptr, FALSE, FALSE, releaseName);
    HANDLE requestEvent = CreateEventW(nullptr, FALSE, FALSE, requestName);
    ASSERT_NE(releaseEvent, nullptr);
    ASSERT_NE(requestEvent, nullptr);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    CloseHandle(releaseEvent);
    CloseHandle(requestEvent);

// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)

    EXPECT_LT(elapsedMs, 100.0);
    EXPECT_EQ(limiter.GetMissedFrames(), 0u);
}

TEST_F(FpsLimiterTest, GeneralBasicDeduplicatesImmediateSequentialApplyWhileActive) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(30);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply();

    bool sawFastDedup = false;
    for (int attempt = 0; attempt < 3 && !sawFastDedup; ++attempt) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply();
        QueryPerformanceCounter(&end);

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        sawFastDedup = elapsedMs < 3.0 && limiter.GetLastWaitUs() == 0;
    }

    EXPECT_TRUE(sawFastDedup);
}

// Strange Brigade Vulkan presents several real swapchain images per frame
// period (concurrent present streams). The legacy 2ms dedup treated the second
// present as a duplicate and let it through unpaced, so the displayed rate was
// 2x the target with alternating short/long frame times. gateEveryPresent must
// pace the immediate second Apply too: it waits for the next grid slot instead
// of returning fast.
TEST_F(FpsLimiterTest, GateEveryPresentPacesImmediateSecondApply) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(false, true);

    bool sawFastDedup = false;
    bool sawPacedSecondApply = false;
    for (int attempt = 0; attempt < 3 && !sawPacedSecondApply; ++attempt) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(false, true);
        QueryPerformanceCounter(&end);

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        sawFastDedup = elapsedMs < 3.0 && limiter.GetLastWaitUs() == 0;
        // Strict grid must never take the dedup fast path: the second present
        // waits for its own grid slot (~16.7ms after the first at 60fps).
        sawPacedSecondApply = elapsedMs >= 3.0 && limiter.GetLastWaitUs() > 0;
    }

    EXPECT_FALSE(sawFastDedup);
    EXPECT_TRUE(sawPacedSecondApply);
}

// FG-scaled modes keep the legacy dedup behavior: generated frames arrive a
// few ms after the base frame and must not be pushed onto the base grid, so
// gateEveryPresent defers to the dedup fast path while FG is active.
TEST_F(FpsLimiterTest, GateEveryPresentDefersToDedupWhileFGActive) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(false, true);

    bool sawFastDedup = false;
    for (int attempt = 0; attempt < 3 && !sawFastDedup; ++attempt) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(false, true);
        QueryPerformanceCounter(&end);

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        sawFastDedup = elapsedMs < 3.0 && limiter.GetLastWaitUs() == 0;
    }

    EXPECT_TRUE(sawFastDedup);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// gateEveryPresent must never stall when the limiter is not configured: it
// only changes lock/dedup semantics, not the inactive fast path.
TEST_F(FpsLimiterTest, GateEveryPresentStaysNonBlockingWhenInactive) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(false);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(false, true);
    QueryPerformanceCounter(&end);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    EXPECT_LT(elapsedMs, 100.0);
    EXPECT_FALSE(limiter.IsActivelyLimiting());
}

TEST_F(FpsLimiterTest, CaptureWarmupUsesCaptureRequestedForCaptureSync) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    limiter.Apply();

// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    EXPECT_GE(elapsedMs, 3.0);
    EXPECT_LT(elapsedMs, 100.0);
}

TEST_F(FpsLimiterTest, VfrCaptureStillHonorsConfiguredGeneralLimiter) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetUseVFR(true);
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply();

    EXPECT_TRUE(limiter.IsActivelyLimiting());
    EXPECT_GT(limiter.GetLastWaitUs(), 0);
}

TEST(FpsLimiterPolicyTest, FrameGenerationScalingMatchesCaptureSource) {
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(false, false));
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(false, true));
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(true, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(true, true));
}

TEST(FpsLimiterPolicyTest, RationalIntervalsPreserveExactLongTermCadence) {
    int64_t remainder = 0;
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalIntervalTicks(10, 3, remainder), 3);
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalIntervalTicks(10, 3, remainder), 3);
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalIntervalTicks(10, 3, remainder), 4);
    EXPECT_EQ(remainder, 0);
}

TEST(FpsLimiterPolicyTest, CaptureSyncLateAdvancePreservesGridPhaseWithoutImmediateCatchup) {
    int64_t remainder = 0;
    const auto result = ce::fps_limiter_policy::AdvanceCaptureSyncDeadlineAfterLateFrame(
        /*currentTargetQpc=*/1000, /*nowQpc=*/1350, /*frequency=*/1000, /*fps=*/10, remainder);

    EXPECT_EQ(result.nextTargetQpc, 1400);
    EXPECT_EQ(result.skippedGridSlots, 4u);
    EXPECT_EQ(result.nextTargetQpc % 100, 0);
    EXPECT_GE(result.nextTargetQpc - 1350, 50);
}

// Test that limiter mode config values are stored/read correctly in shared memory
TEST_F(FpsLimiterTest, LimiterMode_SharedMemory) {
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kFGFallback));
    EXPECT_EQ(mockShm->fpsLimiter.GetCaptureSyncLimiterMode(), 1u);

    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));
    EXPECT_EQ(mockShm->fpsLimiter.GetGeneralLimiterMode(), 2u);

    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));
    EXPECT_EQ(mockShm->fpsLimiter.GetGeneralLimiterMode(), 3u);
}

// Test that FG fallback mode doubles interval via capture sync local limiter
TEST_F(FpsLimiterTest, FGFallback_CaptureSync_DoublesInterval) {
    // Setup capture sync at 60fps with FG fallback mode
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kFGFallback));

    // Simulate FG active (DLSS FG)
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);

    // Call Apply twice: first sets up cadence, second actually waits
    limiter.Apply();  // First call: sets up localTargetTime_

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();  // Second call: should wait ~33ms (60/2 = 30fps = 33.3ms)
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)

    // With FG active and 60fps target, effective is 30fps → ~33ms interval
    // Allow wide margin for scheduling
    EXPECT_GE(elapsedMs, 25.0);  // At least ~25ms (33ms - jitter)
    EXPECT_LT(elapsedMs, 100.0);  // Loaded-host sanity bound

    // Cleanup
    g_FGCompat.SetDLSSFGActive(false);
}

// Test auto mode falls back to basic when no FG and no Reflex
TEST_F(FpsLimiterTest, AutoMode_FallsBackToBasic) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));

    // No FG, no Reflex → auto should resolve to basic
    g_FGCompat.SetDLSSFGActive(false);

// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    limiter.Apply();  // First call: cadence setup

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)

    // Auto → basic: 60fps → ~16.6ms
    EXPECT_GE(elapsedMs, 13.0);
    EXPECT_LT(elapsedMs, 100.0);
}

// Test auto mode uses FG fallback when FG is active but no Reflex
TEST_F(FpsLimiterTest, AutoMode_UsesFGFallbackWhenFGActive) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    // FG active, no Reflex → auto should resolve to fg_fallback
    g_FGCompat.SetFSRFGActive(true);

    limiter.Apply();  // First call: cadence setup

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)

    // Auto → fg_fallback: 60fps / 2 = 30fps → ~33ms
    EXPECT_GE(elapsedMs, 25.0);
    EXPECT_LT(elapsedMs, 100.0);

    g_FGCompat.SetFSRFGActive(false);
}

TEST_F(FpsLimiterTest, InactiveLimiterClearsStaleReflexOverride) {
    g_ReflexLimiter.SetTargetFps(69);
    EXPECT_NE(g_ReflexLimiter.GetTargetIntervalUs(), 0u);

    mockShm->runtimeState.captureRequested = false;
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(false);
    mockShm->fpsLimiter.SetGeneralEnabled(false);
    mockShm->fpsLimiter.SetUseVFR(false);

    limiter.Apply();

    EXPECT_EQ(g_ReflexLimiter.GetTargetIntervalUs(), 0u);
}

TEST_F(FpsLimiterTest, ReflexLimiterTracksPublishedDeviceForNativePacing) {
    g_ReflexLimiter.Shutdown();
    EXPECT_FALSE(g_ReflexLimiter.HasDevice());

    auto* fakeDevice = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(0x1234));
    g_ReflexLimiter.SetDevice(fakeDevice);

    EXPECT_TRUE(g_ReflexLimiter.HasDevice());

    g_ReflexLimiter.SetTargetFps(60);
    EXPECT_EQ(g_ReflexLimiter.GetTargetIntervalUs(), 16666u);

    g_ReflexLimiter.Shutdown();
}

TEST_F(FpsLimiterTest, ReflexSleepModeParamsMatchNvApiAbi) {
    EXPECT_EQ(sizeof(NV_SET_SLEEP_MODE_PARAMS), 44u);
    EXPECT_EQ(NV_SET_SLEEP_MODE_PARAMS_VER, 0x0001002Cu);
    EXPECT_EQ(offsetof(NV_SET_SLEEP_MODE_PARAMS, bLowLatencyMode), 4u);
    EXPECT_EQ(offsetof(NV_SET_SLEEP_MODE_PARAMS, bLowLatencyBoost), 5u);
    EXPECT_EQ(offsetof(NV_SET_SLEEP_MODE_PARAMS, minimumIntervalUs), 8u);
    EXPECT_EQ(offsetof(NV_SET_SLEEP_MODE_PARAMS, bUseMarkersToOptimize), 12u);
    EXPECT_EQ(offsetof(NV_SET_SLEEP_MODE_PARAMS, bUseMinQueueTime), 13u);
    EXPECT_EQ(offsetof(NV_SET_SLEEP_MODE_PARAMS, rsvd), 14u);
}

TEST_F(FpsLimiterTest, ManualReflexFirstPushRearmsLowLatencyModeBeforeLimit) {
    ResetManualRearmSetSleepModeRecorder();

    auto* fakeDevice = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(0x5678));
    g_ReflexLimiter.TestInstallSetSleepModeForUnitTest(&TestManualRearmSetSleepMode, fakeDevice);
    g_ReflexLimiter.SetManualLimiterConfiguredOrActive(true);
    g_ReflexLimiter.SetTargetFps(60);

    EXPECT_TRUE(g_ReflexLimiter.PushFpsLimit());

    ASSERT_EQ(g_TestSetSleepModeCallCount.load(std::memory_order_acquire), 2);
    EXPECT_EQ(g_TestSetSleepModeParams[0].bLowLatencyMode, 0u);
    EXPECT_EQ(g_TestSetSleepModeParams[0].minimumIntervalUs, 0u);
    EXPECT_EQ(g_TestSetSleepModeParams[1].bLowLatencyMode, 1u);
    EXPECT_EQ(g_TestSetSleepModeParams[1].minimumIntervalUs, 16666u);

    EXPECT_TRUE(g_ReflexLimiter.PushFpsLimit());
    EXPECT_EQ(g_TestSetSleepModeCallCount.load(std::memory_order_acquire), 2);

    g_ReflexLimiter.Shutdown();
}

TEST_F(FpsLimiterTest, NonManualReflexPushDoesNotForceLowLatencyReset) {
    ResetManualRearmSetSleepModeRecorder();

    auto* fakeDevice = reinterpret_cast<IUnknown*>(static_cast<uintptr_t>(0x5678));
    g_ReflexLimiter.TestInstallSetSleepModeForUnitTest(&TestManualRearmSetSleepMode, fakeDevice);
    g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
    g_ReflexLimiter.SetTargetFps(60);

    EXPECT_TRUE(g_ReflexLimiter.PushFpsLimit());

    ASSERT_EQ(g_TestSetSleepModeCallCount.load(std::memory_order_acquire), 1);
    EXPECT_EQ(g_TestSetSleepModeParams[0].bLowLatencyMode, 1u);
    EXPECT_EQ(g_TestSetSleepModeParams[0].minimumIntervalUs, 16666u);

    g_ReflexLimiter.Shutdown();
}

TEST(ReflexFpsLimiterPolicyTest, ExplicitReflexLocalCadenceSurvivesPresentGapWithoutGameSleep) {
    const auto decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(true, false, false, 0, true);

    EXPECT_TRUE(decision.useExplicitLocalCadence);
    EXPECT_FALSE(decision.useGameSleepHandoff);
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldRunExplicitReflexCadencePostPresent(decision, true));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldRunExplicitReflexCadencePostPresent(decision, false));
}

TEST(ReflexFpsLimiterPolicyTest, GameOwnedReflexHandoffStillRequiresFreshStableSleepWithoutGap) {
    auto decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(false, true, true, 2, false);
    EXPECT_FALSE(decision.useExplicitLocalCadence);
    EXPECT_FALSE(decision.useGameSleepHandoff);

    decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(false, true, true, 3, true);
    EXPECT_FALSE(decision.useGameSleepHandoff);

    decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(false, true, true, 3, false);
    EXPECT_TRUE(decision.useGameSleepHandoff);
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldRunExplicitReflexCadencePostPresent(decision, true));
}

TEST(ReflexFpsLimiterPolicyTest, ExplicitReflexUsesLocalCadenceUntilGameSleepHandoffIsStable) {
    auto decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(true, true, true, 2, false);
    EXPECT_TRUE(decision.useExplicitLocalCadence);
    EXPECT_FALSE(decision.useGameSleepHandoff);

    decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(true, true, true, 3, true);
    EXPECT_TRUE(decision.useExplicitLocalCadence);
    EXPECT_FALSE(decision.useGameSleepHandoff);

    decision = ce::fps_limiter_policy::ResolveReflexPacingDecision(true, true, true, 3, false);
    EXPECT_FALSE(decision.useExplicitLocalCadence);
    EXPECT_TRUE(decision.useGameSleepHandoff);
}

TEST(ReflexFpsLimiterPolicyTest, NvApiReflexWrapperIsOnlyReturnedForManualGameCallers) {
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(true, false, false, false, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(false, false, false, false, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(true, true, false, false, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(true, false, true, false, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(true, false, false, true, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldReturnNvApiReflexWrapper(true, false, false, false, true));
}

TEST(ReflexFpsLimiterPolicyTest, ManualReflexConfigCanArmQueryHookBeforeNvApiLoads) {
    constexpr uint32_t kBasicMode = static_cast<uint32_t>(LimiterMode::kBasic);
    constexpr uint32_t kNativeMode = static_cast<uint32_t>(LimiterMode::kNative);

    EXPECT_TRUE(
        ce::fps_limiter_policy::IsManualReflexLimiterConfigured(true, 60, kNativeMode, false, kBasicMode, kNativeMode));
    EXPECT_TRUE(
        ce::fps_limiter_policy::IsManualReflexLimiterConfigured(false, 0, kBasicMode, true, kNativeMode, kNativeMode));
    EXPECT_FALSE(
        ce::fps_limiter_policy::IsManualReflexLimiterConfigured(true, 0, kNativeMode, false, kBasicMode, kNativeMode));
    EXPECT_FALSE(
        ce::fps_limiter_policy::IsManualReflexLimiterConfigured(true, 60, kBasicMode, false, kBasicMode, kNativeMode));
}

TEST_F(FpsLimiterTest, FGFallback_UsesExplicitDLSSMultiplier) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kFGFallback));

// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply();

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)

    EXPECT_GE(elapsedMs, 40.0);
    EXPECT_LT(elapsedMs, 100.0);

    g_FGCompat.SetDLSSFGActive(false);
}

TEST_F(FpsLimiterTest, HeuristicFSRPriorityOverTransientDLSSFG) {
    g_FGCompat.SetHeuristicFSRFGActive(true);
    EXPECT_TRUE(g_FGCompat.IsFGActive());
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::FSR_FG);

    // DLSS FG API activation is suppressed while heuristic FSR FG is active
    // (prevents ping-pong from transient Streamline state toggling).
    g_FGCompat.SetDLSSFGActive(true);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::FSR_FG);
    EXPECT_TRUE(g_FGCompat.IsHeuristicFSRFGActive());

    // Deactivating heuristic FSR FG allows DLSS FG to take over.
    g_FGCompat.SetHeuristicFSRFGActive(false);
    EXPECT_FALSE(g_FGCompat.IsHeuristicFSRFGActive());
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::DLSS_FG);

    g_FGCompat.SetDLSSFGActive(false);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::None);
}

TEST_F(FpsLimiterTest, AuthoritativeFSRGetsStableDefaultMultiplier) {
    g_FGCompat.SetFSRFGActive(true);

    EXPECT_TRUE(g_FGCompat.IsFGActive());
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::FSR_FG);
    EXPECT_EQ(g_FGCompat.GetFGMultiplier(), 2);

    g_FGCompat.SetFSRFGActive(false);
}

TEST_F(FpsLimiterTest, DirectFFXConfirmationSurvivesRepeatedAuthoritativeFSRActivationSignals) {
    g_FGCompat.SetFSRFGActive(true);
    g_FGCompat.MarkDirectFFXApiConfirmation();
    EXPECT_TRUE(g_FGCompat.HasDirectFFXApiConfirmation());

    // Repeated authoritative "FG still enabled" updates during the same
    // activation must not wipe the direct API confirmation latch.
    g_FGCompat.SetFSRFGActive(true);
    EXPECT_TRUE(g_FGCompat.HasDirectFFXApiConfirmation());

    g_FGCompat.SetFSRFGActive(false);
    EXPECT_FALSE(g_FGCompat.HasDirectFFXApiConfirmation());
}

TEST_F(FpsLimiterTest, AuthoritativeFSRCanOverrideTransientDLSSAndYieldBackToConfirmedDLSS) {
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::DLSS_FG);

    g_FGCompat.SetFSRFGActive(true);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::FSR_FG);
    EXPECT_EQ(g_FGCompat.GetFGMultiplier(), 2);

    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::DLSS_FG);

    g_FGCompat.SetDLSSFGActive(false);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::None);
}

TEST_F(FpsLimiterTest, AuthoritativeFSROffClearsStaleHeuristic) {
    // Simulate a heuristic latched during an earlier queue-change window.
    g_FGCompat.SetHeuristicFSRFGActive(true);
    EXPECT_TRUE(g_FGCompat.IsHeuristicFSRFGActive());
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::FSR_FG);

    // Authoritative FSR OFF must invalidate the stale heuristic so the overlay
    // is not skipped indefinitely during post-FSR teardown / DLSS comeback.
    g_FGCompat.SetFSRFGActive(false);
    EXPECT_FALSE(g_FGCompat.IsHeuristicFSRFGActive());
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::None);
}

TEST_F(FpsLimiterTest, StaleHeuristicFSRClearedByAuthoritativeFSROffAllowsDLSSReactivation) {
    // Simulate heuristic FSR latched during an active FSR phase, then FSR is
    // turned off authoritatively.  The heuristic must be cleared so that the
    // subsequent DLSS reactivation is not blocked.
    g_FGCompat.SetFSRFGActive(true);
    g_FGCompat.SetHeuristicFSRFGActive(true);
    EXPECT_TRUE(g_FGCompat.IsHeuristicFSRFGActive());
    EXPECT_TRUE(g_FGCompat.IsFSRFGApiActive());

    // Authoritative FSR OFF must clear the heuristic.
    g_FGCompat.SetFSRFGActive(false);
    EXPECT_FALSE(g_FGCompat.IsHeuristicFSRFGActive());
    EXPECT_FALSE(g_FGCompat.IsFSRFGApiActive());

    // DLSS reactivation must now succeed because the stale heuristic is gone.
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::DLSS_FG);

    g_FGCompat.SetDLSSFGActive(false);
    EXPECT_EQ(g_FGCompat.GetActiveFGType(), FGCompatibility::FGType::None);
}

// Test ParseLimiterMode
TEST(LimiterModeParseTest, ParsesAllValues) {
    EXPECT_EQ(ParseLimiterMode("basic"), LimiterMode::kBasic);
    EXPECT_EQ(ParseLimiterMode("fg_fallback"), LimiterMode::kFGFallback);
    EXPECT_EQ(ParseLimiterMode("fallback"), LimiterMode::kFGFallback);
    EXPECT_EQ(ParseLimiterMode("native"), LimiterMode::kNative);
    EXPECT_EQ(ParseLimiterMode("reflex"), LimiterMode::kNative);
    EXPECT_EQ(ParseLimiterMode(" Reflex "), LimiterMode::kNative);
    EXPECT_EQ(ParseLimiterMode("nvidia-reflex"), LimiterMode::kNative);
    EXPECT_EQ(ParseLimiterMode("auto"), LimiterMode::kAuto);
    EXPECT_EQ(ParseLimiterMode(""), LimiterMode::kAuto);         // Default
    EXPECT_EQ(ParseLimiterMode("invalid"), LimiterMode::kAuto);  // Default
}

TEST(OverlayCompatTest, DetectsKnownOverlayModulePaths) {
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyOverlayModulePath("C:\\Games\\GTAV\\socialclub.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyOverlayModulePath("C:\\Games\\GTAV\\SocialClubD3D12Renderer.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsThirdPartyOverlayModulePath("C:\\Games\\GTAV\\EOSOVH_Win64_Shipping.dll"));
    EXPECT_TRUE(
        ce::overlay_compat::IsThirdPartyOverlayModulePath(L"C:\\Program Files\\Epic\\EOSOVH_Win64_Shipping.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsThirdPartyOverlayModulePath("C:\\capture\\capture_hook_x64.dll"));
}

TEST(OverlayCompatTest, EffectiveCreateSwapchainCallerPrefersForwardedExternalCaller) {
    EXPECT_STREQ("C:\\Games\\GTAV\\EOSOVH_Win64_Shipping.dll",
                 ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
                     "C:\\Games\\GTAV\\EOSOVH_Win64_Shipping.dll", "C:\\capture\\capture_hook_x64.dll"));
    EXPECT_STREQ("C:\\Games\\GTAV\\SocialClubD3D12Renderer.dll",
                 ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
                     nullptr, "C:\\Games\\GTAV\\SocialClubD3D12Renderer.dll"));

    EXPECT_TRUE(ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
        "C:\\Games\\GTAV\\EOSOVH_Win64_Shipping.dll", "C:\\capture\\capture_hook_x64.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
        nullptr, "C:\\Games\\GTAV\\SocialClubD3D12Renderer.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
        "C:\\Games\\GTAV\\GTA5_Enhanced.exe", "C:\\Games\\GTAV\\SocialClubD3D12Renderer.dll"));
}

TEST(OverlayCompatTest, StartupBlockingOverlayPathsAreTrackedSeparatelyFromGenericOverlayPaths) {
    EXPECT_TRUE(ce::overlay_compat::IsStartupBlockingOverlayModulePath("C:\\Games\\GTAV\\EOSOVH_Win64_Shipping.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsStartupBlockingOverlayModulePath("C:\\Games\\GTAV\\SocialClubD3D12Renderer.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsStartupBlockingOverlayModulePath(
        "C:\\Program Files\\Epic Games\\GTAVEnhanced\\sl.interposer.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsStartupBlockingOverlayModulePath("C:\\capture\\capture_hook_x64.dll"));
}

TEST(OverlayCompatTest, FFXFrameGenerationModulePathsCoverLegacyAndGenericAMDNames) {
    EXPECT_TRUE(
        ce::overlay_compat::IsFFXFrameGenerationModulePath("C:\\Games\\GTAV\\amd_fidelityfx_framegeneration_dx12.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsFFXFrameGenerationModulePath("C:\\Games\\GTAV\\amd_fidelityfx_dx12.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsFFXFrameGenerationModulePath(L"C:\\Games\\GTAV\\amd_fidelityfx_vk.dll"));

    EXPECT_FALSE(ce::overlay_compat::IsFFXFrameGenerationModulePath("C:\\Program Files\\NVIDIA\\nvngx_dlssg.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsFFXFrameGenerationModulePath("C:\\capture\\capture_hook_x64.dll"));
}

TEST(OverlayCompatTest, StreamlineFrameGenerationModulePathsCoverInterposerAndNVNGXNames) {
    EXPECT_TRUE(ce::overlay_compat::IsStreamlineFrameGenerationModulePath(
        "C:\\Program Files\\Epic Games\\GTAVEnhanced\\sl.interposer.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsStreamlineFrameGenerationModulePath(
        "C:\\Program Files\\Epic Games\\GTAVEnhanced\\sl.dlss_g.dll"));
    EXPECT_TRUE(ce::overlay_compat::IsStreamlineFrameGenerationModulePath(L"C:\\Windows\\System32\\nvngx_dlssg.dll"));

    EXPECT_FALSE(
        ce::overlay_compat::IsStreamlineFrameGenerationModulePath("C:\\Games\\GTAV\\EOSOVH_Win64_Shipping.dll"));
    EXPECT_FALSE(ce::overlay_compat::IsStreamlineFrameGenerationModulePath("C:\\capture\\capture_hook_x64.dll"));
}

TEST(OverlayCompatTest, NullAndInvalidFFXModuleInputsStayRejected) {
    EXPECT_FALSE(ce::overlay_compat::IsFFXFrameGenerationModuleHandle(nullptr));
    EXPECT_FALSE(ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(nullptr));
}

TEST(OverlayCompatTest, StartupOverlaySuppressionTracksVisibleWindowAndCooldown) {
    EXPECT_TRUE(ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(true, false, false, 0, 5000, 5000, 5000));
    EXPECT_TRUE(ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(true, false, true, 6000, 5000, 0, 5000));
    EXPECT_TRUE(ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(true, false, false, 6000, 5000, 1000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(true, false, false, 6000, 5000, 5000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(true, true, false, 0, 5000, 0, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(false, false, false, 0, 5000, 0, 5000));
}

TEST(OverlayCompatTest, ProcessNameNoLongerDrivesStartupInitGrace) {
    EXPECT_FALSE(ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess("GTA5.exe"));
    EXPECT_FALSE(ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess("GTA5_Enhanced.exe"));
    EXPECT_FALSE(ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess("Cyberpunk2077.exe"));
}

TEST(OverlayCompatTest, PostResumeInitDelayRequiresForegroundAndUsableWindow) {
    EXPECT_TRUE(
        ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(true, true, false, true, 1280, 720, 0, 5000));
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(true, true, false, false, 1280, 720,
                                                                                 6000, 5000));
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(true, true, false, true, 320, 200,
                                                                                 6000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(true, true, false, true, 1280, 720,
                                                                                  5000, 5000));
    EXPECT_FALSE(
        ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(true, false, false, true, 1280, 720, 0, 5000));
    EXPECT_FALSE(
        ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(true, true, true, true, 1280, 720, 0, 5000));
}

TEST(OverlayCompatTest, PostResumeOverlayDelayAlsoBlocksRuntimeOwnedSwapchainTransitions) {
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, true, true, 1280, 720,
                                                                             6000, 5000));
    EXPECT_TRUE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, false, false, 1280, 720,
                                                                             6000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, false, false, true, 1280, 720,
                                                                              6000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, true, true, true, true, 1280, 720,
                                                                              6000, 5000));
    EXPECT_FALSE(ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(true, false, false, true, true, 1280, 720,
                                                                              6000, 5000));
}

TEST(OverlayCompatTest, UsableSameProcessForegroundWindowIsAcceptedAfterResumeSettle) {
    LONG width = 0;
    LONG height = 0;
    EXPECT_FALSE(ce::overlay_compat::IsSameProcessWindow(nullptr, GetCurrentProcessId()));
    EXPECT_FALSE(ce::overlay_compat::IsUsableSameProcessForegroundWindow(nullptr, GetCurrentProcessId()));
    EXPECT_FALSE(ce::overlay_compat::IsSameProcessWindow(reinterpret_cast<HWND>(1), GetCurrentProcessId()));
    EXPECT_FALSE(ce::overlay_compat::IsUsableSameProcessForegroundWindow(reinterpret_cast<HWND>(1),
                                                                         GetCurrentProcessId(), &width, &height));
}
