#include "test_fps_limiter_shared.h"

#include <thread>

#include "hook/pacing/runtime_input_site_policy.h"
#include "source_fragment_reader.h"

// PresentSite::kRuntimeInputPresent: the game's own Present into an FG
// runtime's proxy swapchain (FFX proxy Present detour, game thread). It paces
// whole output groups and the runtime's outputs pass while it owns pacing, so
// CE no longer stacks a second pacer behind AMD's on the presenter thread.

namespace {

using ce::fps_limiter_policy::PresentSite;
using ce::fps_limiter_policy::ResolveLocalCadencePlan;
using ce::fps_limiter_policy::ShouldGateEveryApplyOnCadenceGrid;
using ce::fps_limiter_policy::ShouldUseDuplicatePresentWindow;
using namespace ce::runtime_input_site_policy;

inline constexpr PresentSite kRuntimeInputSite = PresentSite::kRuntimeInputPresent;
inline constexpr PresentSite kRuntimeOutputSite = PresentSite::kRuntimeOutputPresent;

}  // namespace

TEST(FpsLimiterRuntimeInputSiteTest, OwnershipIsCountedInPresentsNotTime) {
    EXPECT_FALSE(RuntimeInputOwnsPacing(kNoRuntimeInputSeen));
    EXPECT_TRUE(RuntimeInputOwnsPacing(0));
    EXPECT_TRUE(RuntimeInputOwnsPacing(kOutputsBeforeOwnershipLapses - 1));
    EXPECT_FALSE(RuntimeInputOwnsPacing(kOutputsBeforeOwnershipLapses));
    // Several whole groups at the largest multiplier, never less than one.
    EXPECT_GE(kOutputsBeforeOwnershipLapses, 2 * kMaxFrameGenerationMultiplier);
}

TEST(FpsLimiterRuntimeInputSiteTest, CaptureSourceFollowsTheRuntimeOutputStream) {
    EXPECT_FALSE(RuntimeInputCaptureSourceIsFinalOutput(false, false));
    EXPECT_TRUE(RuntimeInputCaptureSourceIsFinalOutput(false, true));
    EXPECT_TRUE(RuntimeInputCaptureSourceIsFinalOutput(true, false));
}

// One entry per rendered frame on the game thread: strict grid, no duplicate
// window, whole groups on the exact rational grid like a final-output boundary.
TEST(FpsLimiterRuntimeInputSiteTest, SitePacesWholeGroupsOnTheStrictGrid) {
    for (const bool fg : {false, true}) {
        EXPECT_TRUE(ShouldGateEveryApplyOnCadenceGrid(kRuntimeInputSite, fg));
        EXPECT_FALSE(ShouldUseDuplicatePresentWindow(kRuntimeInputSite, fg));
    }
    auto plan = ResolveLocalCadencePlan(130, 43, true, 3, true, kRuntimeInputSite);
    EXPECT_EQ(plan.presentTargetFps, 43);
    EXPECT_EQ(plan.presentCadenceTargetFps, 130);
    EXPECT_EQ(plan.presentCadenceScale, 3);
    EXPECT_EQ(plan.renderCadenceTargetFps, 130);
    EXPECT_EQ(plan.renderCadenceScale, 3);
    // FG off (the runtime presents 1:1): one frame per slot at the target.
    plan = ResolveLocalCadencePlan(120, 120, false, 1, true, kRuntimeInputSite);
    EXPECT_EQ(plan.presentCadenceTargetFps, 120);
    EXPECT_EQ(plan.presentCadenceScale, 1);
}

class FpsLimiterRuntimeInputApplyTest : public FpsLimiterTest {
protected:
    void ConfigureFsrGeneral120() {
        UseVirtualClock();
        mockShm->fpsLimiter.SetGeneralEnabled(true);
        mockShm->fpsLimiter.SetGeneralFps(120);
        mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
        g_FGCompat.SetFSRFGMultiplier(2);
        g_FGCompat.SetFSRFGActive(true);
    }

    void TearDown() override {
        mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, false);
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        FpsLimiterTest::TearDown();
    }
};

// 120 displayed under 2x: the game thread waits one 60 Hz group slot per
// rendered frame, and AMD's two outputs per frame pass without any CE wait.
TEST_F(FpsLimiterRuntimeInputApplyTest, GameThreadPacesGroupsAndRuntimeOutputsPass) {
    ConfigureFsrGeneral120();

    limiter.Apply(true, kRuntimeInputSite);
    auto resolved = limiter.GetResolvedCadence();
    ASSERT_GT(resolved.generation, 0u);
    EXPECT_EQ(resolved.targetFps, 120);
    EXPECT_EQ(resolved.cadenceScale, 2);
    EXPECT_NEAR(resolved.intervalUs, 16666, 2);

    for (int output = 0; output < 2; ++output) {
        EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(true, kRuntimeOutputSite); }), 0);
    }
    EXPECT_EQ(limiter.GetRuntimeInputPassCount(), 2u);
    EXPECT_EQ(limiter.GetResolvedCadence().generation, resolved.generation) << "an output reached the cadence";

    EXPECT_EQ(VirtualTicksOf([&] { limiter.Apply(true, kRuntimeInputSite); }), FirstIntervalTicks(120, 2));
}

// The proxy stops carrying the game's frames: after several whole groups of
// outputs without one proxy Present, the output site paces again; the next
// proxy Present takes ownership back.
TEST_F(FpsLimiterRuntimeInputApplyTest, OwnershipLapsesWithoutProxyPresentsAndReturns) {
    ConfigureFsrGeneral120();
    limiter.Apply(true, kRuntimeInputSite);
    const uint32_t generationAfterInput = limiter.GetResolvedCadence().generation;

    for (uint32_t output = 0; output < kOutputsBeforeOwnershipLapses; ++output) {
        limiter.Apply(true, kRuntimeOutputSite);
    }
    EXPECT_EQ(limiter.GetRuntimeInputPassCount(), kOutputsBeforeOwnershipLapses);
    EXPECT_EQ(limiter.GetResolvedCadence().generation, generationAfterInput);

    limiter.Apply(true, kRuntimeOutputSite);
    EXPECT_EQ(limiter.GetRuntimeInputPassCount(), kOutputsBeforeOwnershipLapses);
    auto resolved = limiter.GetResolvedCadence();
    EXPECT_GT(resolved.generation, generationAfterInput) << "the lapsed output site must pace again";
    EXPECT_EQ(resolved.cadenceScale, 1);

    limiter.Apply(true, kRuntimeInputSite);
    EXPECT_EQ(limiter.GetResolvedCadence().cadenceScale, 2);
    limiter.Apply(true, kRuntimeOutputSite);
    EXPECT_EQ(limiter.GetRuntimeInputPassCount(), kOutputsBeforeOwnershipLapses + 1);
}

// Before the first proxy Present nothing changes: outputs keep today's pacing.
TEST_F(FpsLimiterRuntimeInputApplyTest, OutputsPaceUntilAProxyPresentIsSeen) {
    ConfigureFsrGeneral120();
    limiter.Apply(true, kRuntimeOutputSite);
    EXPECT_EQ(limiter.GetRuntimeInputPassCount(), 0u);
    EXPECT_GT(limiter.GetResolvedCadence().generation, 0u);
}

// Capture sync under inject capture: the proxy site compares against the
// source the runtime's outputs define. Callback-proven outputs are what DX12
// capture records (final: the 120 capture grid is the displayed rate); plain
// application presents mean base capture (120 rendered, unscaled).
TEST_F(FpsLimiterRuntimeInputApplyTest, CaptureSyncInheritsTheRuntimeOutputCaptureSource) {
    ConfigureFsrGeneral120();
    mockShm->fpsLimiter.SetGeneralEnabled(false);
    mockShm->runtimeState.captureRequested = true;
    mockShm->runtimeState.isRecording = true;
    mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
    mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
    mockShm->fpsLimiter.SetCaptureFps(120);
    mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
    mockShm->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, true);

    limiter.Apply(true, kRuntimeOutputSite);
    clock.Advance(TicksFromUs(10000));
    limiter.Apply(true, kRuntimeInputSite);
    auto resolved = limiter.GetResolvedCadence();
    EXPECT_EQ(resolved.targetFps, 120);
    EXPECT_EQ(resolved.cadenceScale, 2) << "final-output capture: 120 displayed = 60 rendered groups";

    for (uint32_t output = 0; output <= kOutputsBeforeOwnershipLapses; ++output) {
        clock.Advance(TicksFromUs(10000));
        limiter.Apply(true, kUniquePresentSite);  // lapses ownership, marks the stream as base
    }
    clock.Advance(TicksFromUs(10000));
    limiter.Apply(true, kRuntimeInputSite);
    resolved = limiter.GetResolvedCadence();
    EXPECT_EQ(resolved.targetFps, 120);
    EXPECT_EQ(resolved.cadenceScale, 1) << "base capture: the capture rate is the rendered rate";
}

// The post-present half belongs to the thread whose Apply() armed it. Under
// FSR FG the runtime presents from its own thread; that thread must never run
// a wait the game thread's Apply() armed (the FFX presenter-thread freeze class).
TEST_F(FpsLimiterRuntimeInputApplyTest, PostPresentWorkRunsOnlyOnTheThreadThatArmedIt) {
    struct SleepCounter {
        int sleeps = 0;
    } counter;
    NativeFpsPacingBackend backend{};
    backend.context = &counter;
    backend.isAvailable = [](void*) { return true; };
    backend.isGameActive = [](void*) { return false; };
    backend.setTargetFps = [](void*, int) { return true; };
    backend.sleep = [](void* context, int64_t* waitUs) {
        ++static_cast<SleepCounter*>(context)->sleeps;
        *waitUs = 0;
        return true;
    };
    backend.clear = [](void*) {};
    backend.name = "post-present thread test backend";
    limiter.SetNativePacingBackend(backend);
    UseVirtualClock();
    mockShm->fpsLimiter.SetGeneralEnabled(true);
    mockShm->fpsLimiter.SetGeneralFps(120);
    mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kNative));

    limiter.Apply(true, kRuntimeInputSite);  // arms the native post-present sleep on this thread
    std::thread presenter([&] { limiter.ApplyPostPresent(); });
    presenter.join();
    EXPECT_EQ(counter.sleeps, 0) << "another thread consumed the game thread's post-present sleep";

    limiter.ApplyPostPresent();
    EXPECT_EQ(counter.sleeps, 1);
    limiter.Shutdown();
}

// Frame-generation release at the proxy site: the game thread is released once
// the runtime presented the frame's generated outputs, bounded by the frame's
// work budget before its deadline (Talos FSR FG 2x, session 20261018_200054:
// the generated Present queued behind the game's next-frame GPU work).
TEST(FpsLimiterRuntimeInputSiteTest, GeneratedOutputGateIsTheProxySiteUnderProducingFrameGeneration) {
    using ce::runtime_input_site_policy::GeneratedOutputsPerFrame;
    using ce::runtime_input_site_policy::ShouldGateReleaseOnGeneratedOutputs;
    EXPECT_EQ(GeneratedOutputsPerFrame(true, 2), 1u);
    EXPECT_EQ(GeneratedOutputsPerFrame(true, 4), 3u);
    EXPECT_EQ(GeneratedOutputsPerFrame(false, 2), 0u);
    EXPECT_EQ(GeneratedOutputsPerFrame(true, 1), 0u);
    EXPECT_TRUE(ShouldGateReleaseOnGeneratedOutputs(kRuntimeInputSite, true, 2, true, false));
    EXPECT_FALSE(ShouldGateReleaseOnGeneratedOutputs(kRuntimeInputSite, false, 1, true, false));
    EXPECT_FALSE(ShouldGateReleaseOnGeneratedOutputs(kRuntimeInputSite, true, 2, false, false));
    EXPECT_FALSE(ShouldGateReleaseOnGeneratedOutputs(kRuntimeInputSite, true, 2, true, true));
    EXPECT_FALSE(ShouldGateReleaseOnGeneratedOutputs(kRuntimeOutputSite, true, 2, true, false));
    EXPECT_FALSE(ShouldGateReleaseOnGeneratedOutputs(PresentSite::kFinalOutputBoundary, true, 2, true, false));
}

class FpsLimiterGatedReleaseTest : public FpsLimiterRuntimeInputApplyTest {
protected:
    static constexpr int64_t kFrameWorkUs = 5'000;

    // Enough measured frames for a work budget; the game works 5 ms per frame.
    void WarmUp() {
        ConfigureFsrGeneral120();
        limiter.SetObservedFrameWorkOverrideUs(kFrameWorkUs);
        for (int frame = 0; frame < 24; ++frame) {
            limiter.Apply(true, kRuntimeInputSite);
            limiter.ApplyPostPresent();
            clock.Advance(TicksFromUs(kFrameWorkUs));
        }
        limiter.Apply(true, kRuntimeInputSite);
        ASSERT_EQ(limiter.GetGatedReleaseOutputsNeeded(), 1u) << "2x: one generated output per frame";
        ASSERT_EQ(limiter.GetRuntimeOutputsPresentedSinceHandoff(), 0u);
    }

    void PresentRuntimeOutputOnAnotherThread() {
        std::thread presenter([&] {
            limiter.Apply(true, kRuntimeOutputSite);
            limiter.ApplyPostPresent();
        });
        presenter.join();
    }
};

TEST_F(FpsLimiterGatedReleaseTest, GeneratedOutputPresentedReleasesTheGameImmediately) {
    WarmUp();
    PresentRuntimeOutputOnAnotherThread();
    EXPECT_EQ(limiter.GetRuntimeOutputsPresentedSinceHandoff(), 1u);
    EXPECT_EQ(VirtualTicksOf([&] { limiter.ApplyPostPresent(); }), 0);
    EXPECT_EQ(limiter.GetGatedReleaseOutputsNeeded(), 0u);
}

// No generated output: released at the bound, the work budget before the next
// deadline - later than the back edge, early enough for the measured frame.
TEST_F(FpsLimiterGatedReleaseTest, WithoutGeneratedOutputsTheReleaseIsBoundedByTheWorkBudget) {
    WarmUp();
    const int64_t waited = VirtualTicksOf([&] { limiter.ApplyPostPresent(); });
    EXPECT_GT(waited, 0);
    EXPECT_LT(waited, FirstIntervalTicks(120, 2) - TicksFromUs(kFrameWorkUs));
    // The next frame still makes its deadline: it waits on the grid, never late.
    clock.Advance(TicksFromUs(kFrameWorkUs));
    EXPECT_GT(VirtualTicksOf([&] { limiter.Apply(true, kRuntimeInputSite); }), 0);
}

// A runtime presenting on the game thread inside the proxy Present: that
// nested present's post-present call is the output's, not the game's release.
TEST_F(FpsLimiterGatedReleaseTest, NestedRuntimePresentOnTheGameThreadDoesNotConsumeTheRelease) {
    WarmUp();
    limiter.Apply(true, kRuntimeOutputSite);  // passes: the proxy Present owns pacing
    EXPECT_EQ(VirtualTicksOf([&] { limiter.ApplyPostPresent(); }), 0);
    EXPECT_EQ(limiter.GetRuntimeOutputsPresentedSinceHandoff(), 1u);
    EXPECT_EQ(limiter.GetGatedReleaseOutputsNeeded(), 1u) << "the nested present consumed the game's release";
    EXPECT_EQ(VirtualTicksOf([&] { limiter.ApplyPostPresent(); }), 0) << "its generated output was presented";
}

// The [OVERLAY COST] line for the FFX proxy Present reported the limiter's own
// waits as CE and AMD cost once the limiter moved into that detour (GTA,
// session 20261018_220700: ceAvgUs=1934 runtimeAvgUs=3728). Both halves are
// measured between the pre-present wait and the post-present release.
TEST(FpsLimiterRuntimeInputSiteTest, ProxyPresentCostExcludesTheLimiterWaits) {
    const std::string proxy = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                                 "hook/d3d12/dx12_hook_ffx_proxy_present.cpp");
    size_t from = 0;
    for (int detour = 0; detour < 2; ++detour) {
        const size_t pace = proxy.find("DX12_PaceFFXProxyApplicationPresent(Flags);", from);
        const size_t costStart = proxy.find("const int64_t costStartUs = PerfLogger::GetQpcUs();", pace);
        const size_t returnStamp = proxy.find("const int64_t returnUs = PerfLogger::GetQpcUs();", costStart);
        const size_t release = proxy.find("g_SharedFpsLimiter.ApplyPostPresent();", returnStamp);
        const size_t observe =
            proxy.find("DX12_ObserveFFXProxyPresentCost(costStartUs, forwardUs, returnUs);", release);
        ASSERT_NE(pace, std::string::npos);
        ASSERT_NE(costStart, std::string::npos);
        ASSERT_NE(returnStamp, std::string::npos);
        ASSERT_NE(release, std::string::npos);
        ASSERT_NE(observe, std::string::npos);
        from = observe + 1;
    }
}
