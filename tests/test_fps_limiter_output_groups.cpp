#include "test_fps_limiter_shared.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

// Output-group admission for real final presentation boundaries (the
// Vulkan/RTX Remix limiter fix). The pure policy tests prove the deterministic
// multiplier-ordinal semantics and the exact rational group cadence; the
// fixture tests prove the wiring inside FpsLimiter::Apply(), including the
// invariants the legacy time-window dedup violated.

namespace {

using ce::fps_limiter_policy::OutputGroupAdmission;
using Decision = ce::fps_limiter_policy::OutputGroupAdmission::Decision;

struct MockNativePacingBackendState {
    bool available = true;
    bool gameActive = false;
    bool setTargetSucceeds = true;
    int targetFps = 0;
    int setTargetCalls = 0;
    int sleepCalls = 0;
    int clearCalls = 0;
};

NativeFpsPacingBackend MakeMockNativePacingBackend(MockNativePacingBackendState* mock) {
    NativeFpsPacingBackend backend{};
    backend.context = mock;
    backend.isAvailable = [](void* context) { return static_cast<MockNativePacingBackendState*>(context)->available; };
    backend.isGameActive = [](void* context) { return static_cast<MockNativePacingBackendState*>(context)->gameActive; };
    backend.setTargetFps = [](void* context, int fps) {
        auto* state = static_cast<MockNativePacingBackendState*>(context);
        ++state->setTargetCalls;
        state->targetFps = fps;
        return state->available && state->setTargetSucceeds;
    };
    backend.sleep = [](void* context, int64_t* waitUs) {
        auto* state = static_cast<MockNativePacingBackendState*>(context);
        ++state->sleepCalls;
        *waitUs = 321;
        return state->available;
    };
    backend.clear = [](void* context) { ++static_cast<MockNativePacingBackendState*>(context)->clearCalls; };
    backend.name = "test native backend";
    return backend;
}

double ElapsedMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end, const LARGE_INTEGER& freq) {
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// Sum of `groups` consecutive rational group intervals at the given frequency.
struct RationalSum {
    int64_t total = 0;
    int64_t remainder = 0;
    int64_t firstInterval = 0;
    int64_t maxIntervalStep = 0;
};

RationalSum SumGroupIntervals(int64_t frequency, int fps, int cadenceScale, int groups) {
    RationalSum sum;
    int64_t remainder = 0;
    int64_t previous = 0;
    for (int i = 0; i < groups; ++i) {
        const int64_t step =
            ce::fps_limiter_policy::NextRationalGroupIntervalTicks(frequency, fps, cadenceScale, remainder);
        if (i == 0) {
            sum.firstInterval = step;
        } else {
            sum.maxIntervalStep = std::max(sum.maxIntervalStep, std::abs(step - previous));
        }
        previous = step;
        sum.total += step;
    }
    sum.remainder = remainder;
    return sum;
}

void ConfigureThreeTimesGeneralCap(SharedMemoryLayout& shm, int outputCap) {
    shm.runtimeState.isRecording = false;
    shm.runtimeState.captureRequested = false;
    shm.fpsLimiter.SetGeneralEnabled(true);
    shm.fpsLimiter.SetGeneralFps(outputCap);
    shm.fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure policy: deterministic ordinal admission
// ---------------------------------------------------------------------------

// The core regression for the Portal RTX escape: with 3x, six consecutive
// real-boundary callbacks must produce pace/pass/pass/pace/pass/pass. No
// timestamps exist in this test at all - the classification must never depend
// on elapsed microseconds, which is what let the legacy 2 ms dedup window
// admit whole extra callback groups (measured ~146 fps against a 130 cap).
TEST(FpsLimiterOutputGroupPolicyTest, ThreeTimesSixCallbackBurstAdmitsExactlyTwoGroupOwners) {
    OutputGroupAdmission admission;
    std::vector<Decision> decisions;
    decisions.reserve(6);
    for (int i = 0; i < 6; ++i) {
        decisions.push_back(admission.Classify(3));
    }
    const std::vector<Decision> expected = {Decision::kPaceGroup,
                                            Decision::kPassGeneratedSlot,
                                            Decision::kPassGeneratedSlot,
                                            Decision::kPaceGroup,
                                            Decision::kPassGeneratedSlot,
                                            Decision::kPassGeneratedSlot};
    EXPECT_EQ(decisions, expected);
}

TEST(FpsLimiterOutputGroupPolicyTest, EveryMultiplierWindowContainsExactlyOneGroupOwner) {
    for (int multiplier = 2; multiplier <= 4; ++multiplier) {
        OutputGroupAdmission admission;
        constexpr int kGroups = 50;
        int owners = 0;
        for (int i = 0; i < multiplier * kGroups; ++i) {
            if (admission.Classify(multiplier) == Decision::kPaceGroup) {
                ++owners;
            }
        }
        EXPECT_EQ(owners, kGroups) << "multiplier " << multiplier;
        EXPECT_EQ(admission.PendingGeneratedSlots(), 0u) << "clean cycle end for multiplier " << multiplier;
    }
}

TEST(FpsLimiterOutputGroupPolicyTest, MultiplierOnePacesEveryCallback) {
    OutputGroupAdmission admission;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(admission.Classify(1), Decision::kPaceGroup);
    }
    EXPECT_EQ(admission.PendingGeneratedSlots(), 0u);
}

TEST(FpsLimiterOutputGroupPolicyTest, ResetMakesNextCallbackCleanGroupOwnerAndReportsPartialGroup) {
    OutputGroupAdmission admission;
    ASSERT_EQ(admission.Classify(3), Decision::kPaceGroup);
    ASSERT_EQ(admission.Classify(3), Decision::kPassGeneratedSlot);
    EXPECT_TRUE(admission.Reset()) << "a partial group was discarded";
    EXPECT_EQ(admission.Classify(3), Decision::kPaceGroup);

    // A multiplier change re-bases through the same reset: a completed cycle
    // reports clean, a partial group reports discarded, and the first callback
    // after either reset owns a slot.
    EXPECT_EQ(admission.Classify(3), Decision::kPassGeneratedSlot);
    EXPECT_EQ(admission.Classify(3), Decision::kPassGeneratedSlot);
    EXPECT_FALSE(admission.Reset()) << "a completed cycle left nothing pending";
    EXPECT_EQ(admission.Classify(4), Decision::kPaceGroup);
    EXPECT_TRUE(admission.Reset()) << "one generated slot was still pending";
    EXPECT_EQ(admission.Classify(2), Decision::kPaceGroup);
    EXPECT_EQ(admission.Classify(2), Decision::kPassGeneratedSlot);
    EXPECT_FALSE(admission.Reset());
    EXPECT_EQ(admission.Classify(4), Decision::kPaceGroup);
    EXPECT_EQ(admission.Classify(4), Decision::kPassGeneratedSlot);
    EXPECT_EQ(admission.Classify(4), Decision::kPassGeneratedSlot);
    EXPECT_EQ(admission.Classify(4), Decision::kPassGeneratedSlot);
    EXPECT_EQ(admission.Classify(4), Decision::kPaceGroup);
}

// ---------------------------------------------------------------------------
// Pure policy: exact rational group cadence
// ---------------------------------------------------------------------------

// 130 fps output cap with 3x FG: the group cadence is 130/3 = 43.333... groups
// per second. Over 130 group intervals the summed QPC ticks equal exactly three
// QPC seconds (the Bresenham remainder distributes the sub-tick fraction), not
// the floored 43 groups/s = 129 output fps the old integer target produced.
TEST(FpsLimiterOutputGroupPolicyTest, ExactRationalGroupCadenceSumsToScaledSecondsWithoutDrift) {
    constexpr int64_t kFrequency = 10000000;  // 10 MHz, typical QPC

    // 130/3: exact over one full remainder cycle.
    const RationalSum sum130 = SumGroupIntervals(kFrequency, 130, 3, 130);
    EXPECT_EQ(sum130.total, 3 * kFrequency);
    EXPECT_EQ(sum130.remainder, 0);
    EXPECT_EQ(sum130.maxIntervalStep, 1) << "consecutive intervals differ by at most one tick";

    // Comparable non-divisible and divisible ratios.
    const RationalSum sum100 = SumGroupIntervals(kFrequency, 100, 3, 100);
    EXPECT_EQ(sum100.total, 3 * kFrequency);
    EXPECT_EQ(sum100.remainder, 0);

    const RationalSum sum141 = SumGroupIntervals(kFrequency, 141, 4, 141);
    EXPECT_EQ(sum141.total, 4 * kFrequency);
    EXPECT_EQ(sum141.remainder, 0);

    // Long-run stability: no accumulated phase error over many cycles.
    const RationalSum sumLong = SumGroupIntervals(kFrequency, 130, 3, 3 * 130);
    EXPECT_EQ(sumLong.total, 9 * kFrequency);

    // Scale 1 degenerates to the plain per-frame interval (130 fps -> 76923).
    const RationalSum sumUnscaled = SumGroupIntervals(kFrequency, 130, 1, 130);
    EXPECT_EQ(sumUnscaled.total, kFrequency);
    EXPECT_EQ(sumUnscaled.firstInterval, kFrequency / 130);
}

TEST(FpsLimiterOutputGroupPolicyTest, DegenerateRationalInputsStaySafe) {
    int64_t remainder = 7;
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalGroupIntervalTicks(0, 130, 3, remainder), 1);
    EXPECT_EQ(remainder, 0);
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalGroupIntervalTicks(10000000, 0, 3, remainder), 1);
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalGroupIntervalTicks(10000000, 130, 0, remainder), 1);
    EXPECT_EQ(ce::fps_limiter_policy::NextRationalGroupIntervalTicks(10000000, 130, 3, remainder),
              10000000 * 3 / 130);
}

TEST(FpsLimiterOutputGroupPolicyTest, AbsurdFrequencyDoesNotOverflowTheScaleMultiplication) {
    constexpr int64_t kHugeFrequency = (INT64_MAX / 4) + 1;  // trips the guard for scale 4
    int64_t remainder = 0;
    const int64_t ticks =
        ce::fps_limiter_policy::NextRationalGroupIntervalTicks(kHugeFrequency, 130, 4, remainder);
    EXPECT_GT(ticks, 0);
    EXPECT_EQ(ticks, kHugeFrequency / 130) << "guard degrades to the unscaled interval";
    EXPECT_EQ(remainder, 0);
}

TEST(FpsLimiterOutputGroupPolicyTest, ResolveHelpersClampAndGateOnCaptureSource) {
    using ce::fps_limiter_policy::ResolveCadenceScaleMultiplier;
    using ce::fps_limiter_policy::ResolveOutputGroupAdmissionMultiplier;

    EXPECT_EQ(ResolveOutputGroupAdmissionMultiplier(false, 4), 1);
    EXPECT_EQ(ResolveOutputGroupAdmissionMultiplier(true, 1), 1);
    EXPECT_EQ(ResolveOutputGroupAdmissionMultiplier(true, 3), 3);
    EXPECT_EQ(ResolveOutputGroupAdmissionMultiplier(true, 7), 4) << "clamped to the supported 2-4 range";

    EXPECT_EQ(ResolveCadenceScaleMultiplier(true, 3, true), 3);
    EXPECT_EQ(ResolveCadenceScaleMultiplier(true, 3, false), 1) << "inject capture sync keeps the base rate";
    EXPECT_EQ(ResolveCadenceScaleMultiplier(false, 3, true), 1);
    EXPECT_EQ(ResolveCadenceScaleMultiplier(true, 9, true), 4);
}

// ---------------------------------------------------------------------------
// Integration through FpsLimiter::Apply()
// ---------------------------------------------------------------------------

// 3x six-callback burst through the real Apply path: exactly two paced group
// owners and four generated-slot passes even when every callback arrives
// back-to-back with effectively identical timestamps. The legacy time-window
// policy paced only the first callback of such a burst and let the whole
// second group through unpaced.
TEST_F(FpsLimiterTest, ThreeTimesSixCallbackBurstPacesExactlyTwoGroups) {
    ConfigureThreeTimesGeneralCap(*mockShm, 240);  // 3x group interval = 12.5 ms
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    const uint32_t pacedBefore = limiter.GetPacedGroupCount();
    const uint32_t generatedBefore = limiter.GetGeneratedSlotPassCount();

    std::vector<double> elapsedMs;
    elapsedMs.reserve(6);
    for (int i = 0; i < 6; ++i) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(false, true);
        QueryPerformanceCounter(&end);
        elapsedMs.push_back(ElapsedMs(start, end, freq));
    }

    // Owners are callbacks 1 and 4; generated passes are 2, 3, 5, 6.
    EXPECT_GE(elapsedMs[0], 3.0) << "first owner takes the half-interval cadence slot (~6.25 ms)";
    EXPECT_LT(elapsedMs[1], 3.0);
    EXPECT_LT(elapsedMs[2], 3.0);
    EXPECT_GE(elapsedMs[3], 8.0) << "second owner waits one full group interval (12.5 ms)";
    EXPECT_LT(elapsedMs[4], 3.0);
    EXPECT_LT(elapsedMs[5], 3.0);
    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 2u);
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount() - generatedBefore, 4u);
    EXPECT_EQ(limiter.GetConcurrentApplySkipCount(), 0u);

    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// With FG off every real-boundary callback is its own group owner: the
// Strange Brigade multi-present fix must keep serializing concurrent presents
// onto the cadence grid one per target interval.
TEST_F(FpsLimiterTest, NoFGRealBoundaryPacesEveryCallback) {
    ConfigureThreeTimesGeneralCap(*mockShm, 120);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);

    const uint32_t pacedBefore = limiter.GetPacedGroupCount();
    for (int i = 0; i < 4; ++i) {
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        limiter.Apply(false, true);
        QueryPerformanceCounter(&end);
        EXPECT_GE(ElapsedMs(start, end, freq), 3.0) << "callback " << i << " must be paced";
    }
    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 4u);
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount(), 0u);
}

// Legacy non-boundary call sites (DXVK Present+PresentEx sequential duplicate)
// keep the immediate dedup while FG is active: generated outputs there are not
// pushed onto the base grid and the dedup fast path still applies.
TEST_F(FpsLimiterTest, LegacyDuplicateCallSiteKeepsImmediateDedupWhileFGActive) {
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(240);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    g_FGCompat.SetDLSSFGMultiplier(2);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply();  // paces the first base frame
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply();  // duplicate of the same logical frame
    QueryPerformanceCounter(&end);

    EXPECT_LT(ElapsedMs(start, end, freq), 3.0);
    EXPECT_EQ(limiter.GetLastWaitUs(), 0);
    EXPECT_EQ(limiter.GetPacedGroupCount(), 0u) << "boundary admission counters stay untouched";

    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// A configured-target change resets the output-group admission: a partial
// group from the old configuration must never leak into the new one, and the
// first callback after the transition owns a clean cadence slot on the new
// group grid.
TEST_F(FpsLimiterTest, TargetChangeResetsGroupAdmissionAndPacesNextCallback) {
    ConfigureThreeTimesGeneralCap(*mockShm, 240);  // group interval 12.5 ms
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(false, true);  // owner
    limiter.Apply(false, true);  // generated pass
    ASSERT_EQ(limiter.GetPacedGroupCount(), 1u);

    const uint32_t resetsBefore = limiter.GetGroupAdmissionResetCount();
    mockShm->fpsLimiter.SetGeneralFps(120);  // group interval 25 ms

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(false, true);  // clean group owner after the reset
    QueryPerformanceCounter(&end);

    EXPECT_GE(ElapsedMs(start, end, freq), 8.0) << "first cadence slot of the new grid (~12.5 ms half-interval)";
    EXPECT_EQ(limiter.GetGroupAdmissionResetCount() - resetsBefore, 1u);
    EXPECT_EQ(limiter.GetPacedGroupCount(), 2u);
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount(), 1u);

    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// Deactivation must clear the group ordinal too so re-activation cannot
// inherit a partial group.
TEST_F(FpsLimiterTest, DeactivationResetsPartialGroupAdmission) {
    ConfigureThreeTimesGeneralCap(*mockShm, 240);
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(false, true);  // owner
    limiter.Apply(false, true);  // generated pass -> partial group pending
    ASSERT_EQ(limiter.GetPacedGroupCount(), 1u);

    mockShm->fpsLimiter.SetGeneralEnabled(false);
    limiter.Apply(false, true);  // inactive fast path

    EXPECT_EQ(limiter.GetGroupAdmissionResetCount(), 1u);
    EXPECT_FALSE(limiter.IsActivelyLimiting());

    // Re-activation: the first callback owns a clean slot.
    const uint32_t resetsBefore = limiter.GetGroupAdmissionResetCount();
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(false, true);
    QueryPerformanceCounter(&end);
    EXPECT_GE(ElapsedMs(start, end, freq), 3.0);
    EXPECT_EQ(limiter.GetGroupAdmissionResetCount() - resetsBefore, 0u) << "already clean from deactivation";
    EXPECT_EQ(limiter.GetPacedGroupCount(), 2u);

    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// Only a pace_group callback may arm the native post-present cadence. A
// generated-slot call must not arm, consume, or execute another call's pending
// post-present state.
TEST_F(FpsLimiterTest, GeneratedSlotsNeverArmPostPresentNativeCadence) {
    MockNativePacingBackendState mock;
    limiter.SetNativePacingBackend(MakeMockNativePacingBackend(&mock));
    mockShm->runtimeState.isRecording = false;
    mockShm->runtimeState.captureRequested = false;
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(240);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(true, true);  // owner: arms the native backend + pending post-present sleep
    EXPECT_EQ(mock.setTargetCalls, 1);
    EXPECT_EQ(mock.targetFps, 80) << "integer driver API keeps the floored base target";
    EXPECT_EQ(mock.sleepCalls, 0);

    limiter.Apply(true, true);  // generated slot
    limiter.Apply(true, true);  // generated slot
    EXPECT_EQ(mock.setTargetCalls, 1) << "generated slots must not arm";
    EXPECT_EQ(mock.sleepCalls, 0);

    limiter.ApplyPostPresent();  // consumes the owner's pending cadence
    EXPECT_EQ(mock.sleepCalls, 1);
    limiter.ApplyPostPresent();  // nothing pending left
    EXPECT_EQ(mock.sleepCalls, 1);

    limiter.CancelPostPresentPacing();
    limiter.Shutdown();
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// Capture semantics: a general/WGC final-output cap paces scaled output groups
// (interval = freq * multiplier / configured target), while inject capture sync
// keeps one base group at the requested capture rate because its source
// contains only application-rendered frames.
TEST_F(FpsLimiterTest, CaptureSourceChoosesGroupCadenceScale) {
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    // WGC-style capture sync observes generated output: 60 fps capture with 3x
    // FG paces 20 groups/s (50 ms group interval, ~25 ms first slot).
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(60);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(false, true);
    QueryPerformanceCounter(&end);
    EXPECT_GE(ElapsedMs(start, end, freq), 15.0);

    // Inject capture sync publishes only application-rendered frames: the
    // target stays the 60 fps base rate (~8.3 ms first slot).
    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);
    mockShm->fpsLimiter.SetGeneralEnabled(false);
    LARGE_INTEGER start2, end2;
    limiter.Shutdown();
    QueryPerformanceCounter(&start2);
    limiter.Apply(false, true);
    QueryPerformanceCounter(&end2);
    const double injectMs = ElapsedMs(start2, end2, freq);
    EXPECT_GE(injectMs, 3.0);
    EXPECT_LT(injectMs, 15.0) << "inject capture sync must not scale the cadence by the FG multiplier";

    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, false);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// A large clock jump (game hitch) advances/resets the cadence without changing
// the modulo admission budget: the same multiplier-cycle continues and no
// extra unpaced group is emitted.
TEST_F(FpsLimiterTest, LateArrivalKeepsGroupAdmissionBudget) {
    ConfigureThreeTimesGeneralCap(*mockShm, 240);  // group interval 12.5 ms
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    limiter.Apply(false, true);  // group 1 owner
    limiter.Apply(false, true);
    limiter.Apply(false, true);
    ASSERT_EQ(limiter.GetPacedGroupCount(), 1u);
    ASSERT_EQ(limiter.GetGeneratedSlotPassCount(), 2u);

    // Simulated game hitch: three group intervals pass before the next owner.
    // The duration only needs to exceed one interval (3x margin), so this is a
    // stall simulation, not a timing assertion.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    const uint32_t pacedBefore = limiter.GetPacedGroupCount();
    const uint32_t generatedBefore = limiter.GetGeneratedSlotPassCount();
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    limiter.Apply(false, true);  // late group-2 owner: deadline already passed
    QueryPerformanceCounter(&end);
    EXPECT_LT(ElapsedMs(start, end, freq), 3.0)
        << "a late owner re-bases the cadence without a short catch-up wait";

    limiter.Apply(false, true);
    limiter.Apply(false, true);
    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 1u) << "exactly one owner per group after the hitch";
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount() - generatedBefore, 2u);

    // The next group's owner paces on the re-based grid.
    QueryPerformanceCounter(&start);
    limiter.Apply(false, true);
    QueryPerformanceCounter(&end);
    EXPECT_GE(ElapsedMs(start, end, freq), 8.0) << "the re-based cadence still paces its group owners";
    limiter.Apply(false, true);
    limiter.Apply(false, true);
    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 2u);
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount() - generatedBefore, 4u);
    EXPECT_EQ(limiter.GetConcurrentApplySkipCount(), 0u);

    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
}

// Concurrent real-boundary callers: the ordinal assigns exactly one owner per
// multiplier callbacks regardless of interleaving, owners serialize onto the
// cadence grid, and zero callers escape through lock contention (the failed
// try_lock no longer admits an arbitrary callback).
TEST_F(FpsLimiterTest, ConcurrentCallersProduceExactGroupOwnersWithoutContentionEscapes) {
    ConfigureThreeTimesGeneralCap(*mockShm, 600);  // 3x group interval = 5 ms
    g_FGCompat.SetDLSSFGMultiplier(3);
    g_FGCompat.SetDLSSFGActive(true);

    constexpr int kThreads = 4;
    constexpr int kCallbacksPerThread = 9;  // 36 callbacks = 12 groups
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    const uint32_t pacedBefore = limiter.GetPacedGroupCount();
    const uint32_t generatedBefore = limiter.GetGeneratedSlotPassCount();
    const uint32_t skipsBefore = limiter.GetConcurrentApplySkipCount();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                // Bounded startup spin: all threads release within microseconds.
            }
            for (int i = 0; i < kCallbacksPerThread; ++i) {
                limiter.Apply(false, true);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(limiter.GetPacedGroupCount() - pacedBefore, 12u);
    EXPECT_EQ(limiter.GetGeneratedSlotPassCount() - generatedBefore, 24u);
    EXPECT_EQ(limiter.GetConcurrentApplySkipCount() - skipsBefore, 0u)
        << "a real-boundary limiter must never lose a callback to lock contention";

    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
    limiter.Shutdown();
}
