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

// Test the high-precision wait logic
TEST_F(FpsLimiterTest, SmartWait_Accuracy) {
    // Target 16ms from now (approx 60 FPS)
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    int64_t targetUs = 16666;  // 16.666 ms
    int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

    // This should block until targetTicks
    bool waited = limiter.SmartWait(targetTicks);

    QueryPerformanceCounter(&end);

    EXPECT_TRUE(waited);

    // Check error margin
    int64_t elapsedTicks = end.QuadPart - start.QuadPart;
    double elapsedMs = (double)elapsedTicks * 1000.0 / freq.QuadPart;

    // Should be at least 16.66ms
    EXPECT_GE(elapsedMs, 16.0);
    // Should not be excessively late (allow 1.5ms scheduling jitter)
    EXPECT_LT(elapsedMs, 18.2);
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

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    EXPECT_GE(elapsedMs, 3.0);   // Should wait at least ~3ms
    EXPECT_LT(elapsedMs, 20.0);  // But not too much more
}

// Test Apply with targetTimeTicks set (uses SmartWait path)
TEST_F(FpsLimiterTest, Apply_WithTargetTimeTicks) {
    // Setup for general FPS limit
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);

    // Set a target time 5ms in the future - this triggers SmartWait
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    mockShm->fpsLimiter.targetTimeTicks.store(now.QuadPart + (5 * freq.QuadPart / 1000), std::memory_order_release);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Should wait approximately 5ms via SmartWait
    EXPECT_GE(elapsedMs, 3.0);
    EXPECT_LT(elapsedMs, 30.0);
}

// Test Apply timeout behavior when target is 0
TEST_F(FpsLimiterTest, Apply_NoTarget_ReturnsImmediately) {
    // Setup for general FPS limit
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);

    // NOTE: When dbgShm is set but targetTimeTicks is 0, the Apply() function
    // returns immediately without waiting. This is the expected behavior.
    // The fallback spin-wait path is only entered when dbgShm is NULL.

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Should return almost immediately (< 5ms)
    EXPECT_LT(elapsedMs, 5.0);
}

TEST_F(FpsLimiterTest, CaptureWarmupUsesCaptureRequestedForCaptureSync) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    mockShm->fpsLimiter.targetTimeTicks.store(now.QuadPart + (5 * freq.QuadPart / 1000), std::memory_order_release);

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    EXPECT_GE(elapsedMs, 3.0);
    EXPECT_LT(elapsedMs, 30.0);
}

// Test that limiter mode config values are stored/read correctly in shared memory
TEST_F(FpsLimiterTest, LimiterMode_SharedMemory) {
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kFGFallback));
    EXPECT_EQ(mockShm->fpsLimiter.GetCaptureSyncLimiterMode(), 1u);

    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));
    EXPECT_EQ(mockShm->fpsLimiter.GetGeneralLimiterMode(), 2u);

    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));
    EXPECT_EQ(mockShm->fpsLimiter.GetGeneralLimiterMode(), 3u);

    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kAntiLag2));
    EXPECT_EQ(mockShm->fpsLimiter.GetCaptureSyncLimiterMode(), 4u);

    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kXeLL));
    EXPECT_EQ(mockShm->fpsLimiter.GetGeneralLimiterMode(), 5u);
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

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();  // Second call: should wait ~33ms (60/2 = 30fps = 33.3ms)
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // With FG active and 60fps target, effective is 30fps → ~33ms interval
    // Allow wide margin for scheduling
    EXPECT_GE(elapsedMs, 25.0);  // At least ~25ms (33ms - jitter)
    EXPECT_LT(elapsedMs, 45.0);  // Less than ~45ms

    // Cleanup
    g_FGCompat.SetDLSSFGActive(false);
}

// Test basic mode ignores FG (no interval doubling)
TEST_F(FpsLimiterTest, BasicMode_IgnoresFG) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    // Simulate FG active
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply();  // First call: cadence setup

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();  // Second call: should wait ~16.6ms (60fps, no halving)
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Basic mode: 60fps target stays 60fps → ~16.6ms interval
    EXPECT_GE(elapsedMs, 13.0);
    EXPECT_LT(elapsedMs, 22.0);

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

    limiter.Apply();  // First call: cadence setup

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Auto → basic: 60fps → ~16.6ms
    EXPECT_GE(elapsedMs, 13.0);
    EXPECT_LT(elapsedMs, 22.0);
}

// Test auto mode uses FG fallback when FG is active but no Reflex
TEST_F(FpsLimiterTest, AutoMode_UsesFGFallbackWhenFGActive) {
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kAuto));

    // FG active, no Reflex → auto should resolve to fg_fallback
    g_FGCompat.SetFSRFGActive(true);

    limiter.Apply();  // First call: cadence setup

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Auto → fg_fallback: 60fps / 2 = 30fps → ~33ms
    EXPECT_GE(elapsedMs, 25.0);
    EXPECT_LT(elapsedMs, 45.0);

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

    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply();

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();
    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    EXPECT_GE(elapsedMs, 40.0);
    EXPECT_LT(elapsedMs, 70.0);

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
    EXPECT_EQ(ParseLimiterMode("anti_lag2"), LimiterMode::kAntiLag2);
    EXPECT_EQ(ParseLimiterMode("antilag2"), LimiterMode::kAntiLag2);
    EXPECT_EQ(ParseLimiterMode("Anti-Lag2"), LimiterMode::kAntiLag2);
    EXPECT_EQ(ParseLimiterMode("xell"), LimiterMode::kXeLL);
    EXPECT_EQ(ParseLimiterMode("intel"), LimiterMode::kXeLL);
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
}

TEST(OverlayCompatTest, DedicatedQueueSupportsFGAndStartupCompat) {
    EXPECT_FALSE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(false, false, nullptr));
    EXPECT_TRUE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(true, false, nullptr));
    // Dedicated queue is now FG-only; startup compat uses single-queue mode to avoid
    // cross-queue resource state conflicts (ERR_GFX_STATE in GTA5 Enhanced).
    EXPECT_FALSE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(false, true, "socialclub.dll"));
    EXPECT_FALSE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(false, true, nullptr));
    EXPECT_TRUE(ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(true, false, "sl.interposer.dll"));
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
