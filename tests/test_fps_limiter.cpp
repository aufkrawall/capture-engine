#include "test_fps_limiter_shared.h"

#include <algorithm>
#include <string>
#include <vector>

// The upper bound every "did not block" assertion in this file uses.
//
// What those assertions exist to catch is a blocking wait on the remote-limiter
// release event, which would cost that event's whole timeout - hundreds of
// milliseconds, not tens. They used to bound elapsed time at 100 ms, which never
// discriminated that from an ordinary scheduling stall on a busy host, and made
// this suite fail roughly one full run in six on a healthy tree. Each site also
// records its elapsed time, so a real slowdown stays visible without being fatal.
//
// This is a weaker timing assumption, not the absence of one. The exact form is
// to assert on what the limiter decided rather than on what the scheduler
// delivered - GateEveryPresentStaysNonBlockingWhenInactive does that with
// GetLastWaitUs() == 0 - and that is where the rest of these belong too.
constexpr double kNotBlockingMs = 500.0;

// Test the high-precision wait logic
// SmartWait's contract is the deadline: it must block until the target tick and
// must not return before it. That holds under any load, so it is asserted
// exactly, per sample, with no margin.
//
// What is deliberately NOT asserted is how far past the deadline each wait
// landed. That number is the host scheduler's answer, not CE's - the previous
// form of this test bounded the median at 20 ms and the worst sample at 60 ms
// over a 16.666 ms wait, and failed on a healthy tree whenever the machine was
// busy. The overshoot is still computed and reported on failure, as a
// diagnostic; the path SmartWait chose to get there is pinned structurally by
// SubTickWaitsLandWithoutTheKernelTimer and its supra-tick sibling.
TEST_F(FpsLimiterTest, SmartWait_Accuracy) {
    limiter.ResetSmartWaitCounters();

    std::vector<double> overshootMs;
    overshootMs.reserve(7);
    for (int i = 0; i < 7; ++i) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);

        const int64_t targetUs = 16666;  // 16.666 ms
        const int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

        ASSERT_TRUE(limiter.SmartWait(targetTicks));

        QueryPerformanceCounter(&end);
        // The deadline, exactly. Never early, at any load.
        EXPECT_GE(end.QuadPart, targetTicks) << "SmartWait returned before its deadline on sample " << i;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        overshootMs.push_back(static_cast<double>(end.QuadPart - targetTicks) * 1000.0 / freq.QuadPart);
    }

    // Each one had room for the kernel timer and must have used it.
    EXPECT_EQ(limiter.GetSmartWaitCount(), 7u);
    EXPECT_EQ(limiter.GetKernelTimerWaitCount(), 7u);

    std::sort(overshootMs.begin(), overshootMs.end());
    RecordProperty("medianOvershootMs", std::to_string(overshootMs[overshootMs.size() / 2]));
    RecordProperty("worstOvershootMs", std::to_string(overshootMs.back()));
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
    EXPECT_GE(elapsedMs, 3.0);  // Should wait at least ~3ms
    RecordProperty("elapsedMs", std::to_string(elapsedMs));
    // See kNotBlockingMs.
    EXPECT_LT(elapsedMs, kNotBlockingMs);
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
    RecordProperty("elapsedMs", std::to_string(elapsedMs));
    // See kNotBlockingMs.
    EXPECT_LT(elapsedMs, kNotBlockingMs);
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
    RecordProperty("elapsedMs", std::to_string(elapsedMs));
    // See kNotBlockingMs.
    EXPECT_LT(elapsedMs, kNotBlockingMs);
}

TEST_F(FpsLimiterTest, GeneralBasicUsesLocalCadenceWithoutLimiterProcessTimeout) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(140);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

// NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)

    RecordProperty("elapsedMs", std::to_string(elapsedMs));
    // See kNotBlockingMs.
    EXPECT_LT(elapsedMs, kNotBlockingMs);
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
// 2x the target with alternating short/long frame times. A final-output site must
// pace the immediate second Apply too: it waits for the next grid slot instead
// of returning fast.
TEST_F(FpsLimiterTest, GateEveryPresentPacesImmediateSecondApply) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(false, kFinalOutputSite);

    bool sawFastDedup = false;
    bool sawPacedSecondApply = false;
    for (int attempt = 0; attempt < 3 && !sawPacedSecondApply; ++attempt) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(false, kFinalOutputSite);
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

// A final-output site must never stall when the limiter is not configured: it
// only changes lock/dedup semantics, not the inactive fast path.
TEST_F(FpsLimiterTest, GateEveryPresentStaysNonBlockingWhenInactive) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(false);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(false, kFinalOutputSite);
    QueryPerformanceCounter(&end);

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    RecordProperty("elapsedMs", std::to_string(elapsedMs));
    // The exact, load-independent form of "non-blocking": an inactive limiter
    // decides to wait for nothing at all, whatever the host was doing meanwhile.
    EXPECT_EQ(limiter.GetLastWaitUs(), 0);
    // See kNotBlockingMs.
    EXPECT_LT(elapsedMs, kNotBlockingMs);
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
    RecordProperty("elapsedMs", std::to_string(elapsedMs));
    // See kNotBlockingMs.
    EXPECT_LT(elapsedMs, kNotBlockingMs);
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
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(false, false, false));
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(false, true, false));
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(true, false, false));
    EXPECT_FALSE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(true, true, false));
    EXPECT_TRUE(ce::fps_limiter_policy::ShouldScaleTargetForFrameGeneration(true, true, true));
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
        /*currentTargetQpc=*/1000, /*nowQpc=*/1350, /*frequency=*/1000, /*fps=*/10, /*cadenceScale=*/1, remainder);

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

// These four tests pin MODE RESOLUTION: which effective rate the limiter
// derives from the configured mode, the frame generation state and capture
// sync. They used to assert on the wall-clock duration of the second Apply(),
// which made them fail under host load for a reason unrelated to what they
// test - RunLocalCadence waits `localTargetTime_ - now`, so every microsecond
// the host spends between arming the deadline and reaching it is subtracted
// from the measured wait. Under load that residual collapses toward zero (and
// past it, at which point the deadline is re-based and nothing is waited for
// at all), so both the lower and the upper bound were load-sensitive.
//
// GetResolvedCadence() reports the decision itself, which is a pure function
// of the configuration. Asserting on it is exact rather than approximate, needs
// no margins, and cannot be perturbed by anything else running on the machine.

// FG fallback halves the effective rate: capture sync at 60 with 2x DLSS FG
// paces the game at 30.
TEST_F(FpsLimiterTest, FGFallback_CaptureSync_DoublesInterval) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kFGFallback));

    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);
    ConfirmDLSSFGPacing();

    limiter.Apply();

    const auto resolved = limiter.GetResolvedCadence();
    EXPECT_GT(resolved.generation, 0u) << "Apply() did not reach the local cadence at all";
    // 60 fps of capture sync, halved by 2x frame generation.
    EXPECT_EQ(resolved.targetFps, 30);
    EXPECT_EQ(resolved.cadenceScale, 1);
    EXPECT_NEAR(resolved.intervalUs, 33333, 100);

    g_FGCompat.SetDLSSFGActive(false);
}

// Auto with neither FG nor Reflex resolves to basic, which paces at the
// configured rate itself.
TEST_F(FpsLimiterTest, AutoMode_FallsBackToBasic) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));

    g_FGCompat.SetDLSSFGActive(false);

    limiter.Apply();

    const auto resolved = limiter.GetResolvedCadence();
    EXPECT_GT(resolved.generation, 0u) << "Apply() did not reach the local cadence at all";
    EXPECT_EQ(resolved.targetFps, 60);
    EXPECT_EQ(resolved.cadenceScale, 1);
    EXPECT_NEAR(resolved.intervalUs, 16666, 100);
}

// Auto with FG active resolves to fg_fallback, so the same 60 becomes 30.
// Paired with the test above, this is the discriminator: same configuration,
// FG state alone decides.
TEST_F(FpsLimiterTest, AutoMode_UsesFGFallbackWhenFGActive) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));

    g_FGCompat.SetFSRFGActive(true);

    limiter.Apply();

    const auto resolved = limiter.GetResolvedCadence();
    EXPECT_GT(resolved.generation, 0u) << "Apply() did not reach the local cadence at all";
    EXPECT_EQ(resolved.targetFps, 30);
    EXPECT_EQ(resolved.cadenceScale, 1);
    EXPECT_NEAR(resolved.intervalUs, 33333, 100);

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

// The divisor is the runtime's reported multiplier, not a fixed 2: at 3x DLSS
// FG, 60 fps of capture sync paces the game at 20.
TEST_F(FpsLimiterTest, FGFallback_UsesExplicitDLSSMultiplier) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kFGFallback));

    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);
    ConfirmDLSSFGPacing();

    limiter.Apply();

    const auto resolved = limiter.GetResolvedCadence();
    EXPECT_GT(resolved.generation, 0u) << "Apply() did not reach the local cadence at all";
    EXPECT_EQ(resolved.targetFps, 20);
    EXPECT_EQ(resolved.cadenceScale, 1);
    EXPECT_NEAR(resolved.intervalUs, 50000, 100);

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
