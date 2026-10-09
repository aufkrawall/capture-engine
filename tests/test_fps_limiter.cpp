#include "test_fps_limiter_shared.h"
#include "test_fps_limiter_sleep_mode_recorder.h"
#include "source_fragment_reader.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

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
// diagnostic. Neither is whether a wait happened at all, nor which path it took:
// a host stall between reading the clock and entering SmartWait can consume
// the whole wait, so on a loaded machine a 16.7 ms request may reach SmartWait
// already due. The path choice is pinned exactly, against the pure policy
// SmartWait uses, by SmartWaitPathIsChosenByRemainingTime.
TEST_F(FpsLimiterTest, SmartWait_Accuracy) {
    limiter.ResetSmartWaitCounters();

    std::vector<double> overshootMs;
    overshootMs.reserve(7);
    for (int i = 0; i < 7; ++i) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);

        const int64_t targetUs = 16666;  // 16.666 ms
        const int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

        limiter.SmartWait(targetTicks);

        QueryPerformanceCounter(&end);
        // The deadline, exactly. Never early, at any load.
        EXPECT_GE(end.QuadPart, targetTicks) << "SmartWait returned before its deadline on sample " << i;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        overshootMs.push_back(static_cast<double>(end.QuadPart - targetTicks) * 1000.0 / freq.QuadPart);
    }

    // Load-proof bookkeeping only: a wait the host delayed past its deadline
    // is not counted, and only counted waits can arm the timer.
    EXPECT_LE(limiter.GetSmartWaitCount(), 7u);
    EXPECT_LE(limiter.GetKernelTimerWaitCount(), limiter.GetSmartWaitCount());
    RecordProperty("kernelTimerWaits", std::to_string(limiter.GetKernelTimerWaitCount()));

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

// Test the SmartWait function directly. The deadline is the whole contract:
// never early, asserted exactly. How late it lands is the scheduler's answer.
TEST_F(FpsLimiterTest, SmartWait_WithTarget) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    const int64_t targetTicks = start.QuadPart + (5 * freq.QuadPart / 1000);
    limiter.SmartWait(targetTicks);

    QueryPerformanceCounter(&end);
    EXPECT_GE(end.QuadPart, targetTicks) << "SmartWait returned before its deadline";
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    RecordProperty("elapsedMs", std::to_string((double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart));
}

// The Apply() tests below run on the virtual clock (UseVirtualClock), so the
// ticks a call took are exactly the wait the limiter decided on. A wait on a
// helper-process event - the "blocking" these tests guard against - is not on
// that clock at all and would show up as a different wait, not as a slow one.

TEST_F(FpsLimiterTest, Apply_GeneralBasicUsesLocalCadence) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);

    // First local-cadence frame starts half an interval ahead.
    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(); }), FirstSlotTicks(60));
    EXPECT_TRUE(limiter.IsActivelyLimiting());
}

TEST_F(FpsLimiterTest, Apply_NoExternalTargetUsesLocalCadence) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);

    // Local cadence waits for its own slot and nothing else.
    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(); }), FirstSlotTicks(60));
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60);
}

TEST_F(FpsLimiterTest, GeneralBasicUsesLocalCadenceWithoutLimiterProcessTimeout) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(140);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(); }), FirstSlotTicks(140));
    EXPECT_EQ(limiter.GetMissedFrames(), 0u);
}

TEST_F(FpsLimiterTest, GeneralBasicDeduplicatesImmediateSequentialApplyWhileActive) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(30);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply();

    // No time passes between the two calls, so the second is a duplicate.
    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(); }), 0);
    EXPECT_EQ(limiter.GetLastWaitUs(), 0);
}

// Strange Brigade Vulkan presents several real swapchain images per frame
// period (concurrent present streams). The legacy 2ms dedup treated the second
// present as a duplicate and let it through unpaced, so the displayed rate was
// 2x the target with alternating short/long frame times. A final-output site must
// pace the immediate second Apply too: it waits for the next grid slot instead
// of returning fast.
TEST_F(FpsLimiterTest, GateEveryPresentPacesImmediateSecondApply) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(60);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    limiter.Apply(false, kFinalOutputSite);

    // Strict grid never takes the dedup fast path: the second present waits
    // for its own grid slot, one interval after the first.
    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(false, kFinalOutputSite); }), FirstIntervalTicks(60));
    EXPECT_GT(limiter.GetLastWaitUs(), 0);
}

// A final-output site must never stall when the limiter is not configured: it
// only changes lock/dedup semantics, not the inactive fast path.
TEST_F(FpsLimiterTest, GateEveryPresentStaysNonBlockingWhenInactive) {
    UseVirtualClock();
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(false);

    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(false, kFinalOutputSite); }), 0);
    EXPECT_EQ(limiter.GetLastWaitUs(), 0);
    EXPECT_FALSE(limiter.IsActivelyLimiting());
}

TEST_F(FpsLimiterTest, CaptureWarmupUsesCaptureRequestedForCaptureSync) {
    UseVirtualClock();
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);

    // Capture sync is already pacing during warmup (captureRequested, not yet
    // recording): the first frame takes the 60 fps half-interval slot.
    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(); }), FirstSlotTicks(60));
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60);
}

TEST_F(FpsLimiterTest, VfrCaptureStillHonorsConfiguredGeneralLimiter) {
    UseVirtualClock();
    mockShm->runtimeState.captureRequested = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetUseVFR(true);
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));

    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(); }), FirstSlotTicks(120));
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

// witcher3windownotappear (0.1.7058): nvapi64.dll was mapped before the hook thread armed, so CE inline-hooked
// the export and published the trampoline; NGX's later LoadLibrary("nvapi64.dll") ran Init() on the game
// thread, which stored the (now patched) export over the trampoline. The detour then forwarded to itself
// through its own patch: the game's main thread and the hook thread both spun there and no window appeared.
TEST(ReflexFpsLimiterPolicyTest, ExportedQueryInterfaceNeverReplacesAPublishedTrampoline) {
    const int trampoline = 0, exported = 0, staleRaw = 0;
    using ce::fps_limiter_policy::ShouldAdoptExportedNvApiQueryInterface;

    EXPECT_TRUE(ShouldAdoptExportedNvApiQueryInterface(nullptr, &exported, false));    // first resolution
    EXPECT_FALSE(ShouldAdoptExportedNvApiQueryInterface(&exported, &exported, false));  // nothing to change
    EXPECT_TRUE(ShouldAdoptExportedNvApiQueryInterface(&staleRaw, &exported, false));   // export moved, no hook
    // The regression: a published trampoline is not replaced by the export, which is CE's own patched entry.
    EXPECT_FALSE(ShouldAdoptExportedNvApiQueryInterface(&trampoline, &exported, true));
    EXPECT_FALSE(ShouldAdoptExportedNvApiQueryInterface(&exported, &exported, true));
    // No export to adopt: keep whatever is stored (the old code nulled it and then reported failure).
    EXPECT_FALSE(ShouldAdoptExportedNvApiQueryInterface(&trampoline, nullptr, false));
    EXPECT_FALSE(ShouldAdoptExportedNvApiQueryInterface(nullptr, nullptr, false));
}

// Init() is the one writer of origQueryInterface_ that used to be unconditional; keep it routed through the
// policy, and the store a compare-exchange so a trampoline another thread publishes in between survives.
TEST(ReflexFpsLimiterPolicyTest, InitStoresTheExportedQueryInterfaceOnlyThroughThePolicy) {
    namespace fs = std::filesystem;
    const std::string init =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "pacing" / "reflex_limiter_detail" / "nvapi_hooks.h");
    ASSERT_FALSE(init.empty());
    const size_t initBody = init.find("inline bool ReflexLimiter::Init() {");
    const size_t initEnd = init.find("inline void ReflexLimiter::EnsureNvAPIHooksInstalled()");
    ASSERT_NE(initBody, std::string::npos);
    ASSERT_NE(initEnd, std::string::npos);
    const std::string body = init.substr(initBody, initEnd - initBody);

    EXPECT_EQ(body.find("origQueryInterface_ =\n"), std::string::npos);
    EXPECT_EQ(body.find("origQueryInterface_ = reinterpret_cast"), std::string::npos);
    const size_t policy = body.find("ShouldAdoptExportedNvApiQueryInterface(");
    const size_t store = body.find("__atomic_compare_exchange_n(&origQueryInterface_");
    const size_t firstUse = body.find("origQueryInterface_(NVAPI_ID_D3D_SetSleepMode)");
    ASSERT_NE(policy, std::string::npos);
    ASSERT_NE(store, std::string::npos);
    ASSERT_NE(firstUse, std::string::npos);
    EXPECT_LT(policy, store);
    EXPECT_LT(store, firstUse);
    // The trampoline is recognised by identity, which is published before origQueryInterface_ itself.
    EXPECT_NE(body.find("observed == directQueryInterfaceTrampoline_"), std::string::npos);
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
