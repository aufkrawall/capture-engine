#include "test_fps_limiter_shared.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

#include "hook/pacing/interposer_output_pacing.h"
#include "hook/pacing/runtime_input_site_policy.h"

// PresentSite::kInterposerOutputPresent: NVIDIA Smooth Motion's outputs, where CE cannot reach the application's Present.
// Trine 5 DX11 under a 140 cap (20261106_181807) waited on one output per frame and the display read stddev 1315 us;
// every output on its own slot read 133-237 us (20261106_151156). See ce::interposer_output_pacing.

namespace {

using ce::fg_runtime::RuntimeMode;
using ce::fps_limiter_policy::PresentSite;
using ce::fps_limiter_policy::ResolveDxgiPresentSite;
using ce::fps_limiter_policy::ResolveEvaluationsPerGameFrame;
using ce::fps_limiter_policy::ResolveLocalCadencePlan;
using ce::fps_limiter_policy::ShouldGateEveryApplyOnCadenceGrid;
using ce::fps_limiter_policy::ShouldUseDuplicatePresentWindow;
using namespace ce::interposer_output_pacing;

inline constexpr PresentSite kOutputSite = PresentSite::kInterposerOutputPresent;
inline constexpr PresentSite kInputSite = PresentSite::kRuntimeInputPresent;

TEST(InterposerOutputGroupsTest, AWorkersPairIsOneGroup) {
    OutputGroupTracker groups;
    const std::vector<uint32_t> indices = {groups.Next(11, 2), groups.Next(11, 2), groups.Next(22, 2), groups.Next(22, 2),
                                           groups.Next(33, 2), groups.Next(33, 2)};
    EXPECT_EQ(indices, (std::vector<uint32_t>{0, 1, 0, 1, 0, 1}));
}

// Runs of 4 and 6 at 2x in the Trine 5 sessions: a worker that got two frames in a row.
TEST(InterposerOutputGroupsTest, TwoFramesInARowOnOneWorkerAreTwoGroups) {
    OutputGroupTracker groups;
    std::vector<uint32_t> indices;
    for (int output = 0; output < 4; ++output) {
        indices.push_back(groups.Next(11, 2));
    }
    EXPECT_EQ(indices, (std::vector<uint32_t>{0, 1, 0, 1}));
}

// A lost output must not shift every later frame by one: the next worker opens its own group.
TEST(InterposerOutputGroupsTest, AThreadChangeOpensAGroupWhateverTheCountSays) {
    OutputGroupTracker groups;
    EXPECT_EQ(groups.Next(11, 2), 0u);
    EXPECT_EQ(groups.Next(22, 2), 0u);
    EXPECT_EQ(groups.Next(22, 2), 1u);
}

TEST(InterposerOutputGroupsTest, ADedicatedPresenterIsCounted) {
    OutputGroupTracker groups;
    std::vector<uint32_t> indices;
    for (int output = 0; output < 6; ++output) {
        indices.push_back(groups.Next(11, 3));
    }
    EXPECT_EQ(indices, (std::vector<uint32_t>{0, 1, 2, 0, 1, 2}));
}

TEST(InterposerOutputGroupsTest, WithoutGenerationEveryOutputIsItsOwnGroup) {
    OutputGroupTracker groups;
    EXPECT_EQ(groups.Next(11, 1), 0u);
    EXPECT_EQ(groups.Next(11, 1), 0u);
    EXPECT_EQ(groups.Next(11, 0), 0u);
}

TEST(InterposerOutputGroupsTest, ResetForgetsThePartialGroup) {
    OutputGroupTracker groups;
    EXPECT_EQ(groups.Next(11, 2), 0u);
    groups.Reset();
    EXPECT_EQ(groups.Next(11, 2), 0u);
}

TEST(InterposerGroupScheduleTest, LaterOutputsDivideTheGroupEvenly) {
    GroupSchedule schedule;
    schedule.Arm(1000, 1300, 3);
    EXPECT_EQ(schedule.OutputDeadline(1), 1100);
    EXPECT_EQ(schedule.OutputDeadline(2), 1200);
}

TEST(InterposerGroupScheduleTest, NothingArmedOrOutsideTheGroupHasNoSlot) {
    GroupSchedule schedule;
    EXPECT_EQ(schedule.OutputDeadline(1), 0);
    schedule.Arm(1000, 1300, 3);
    EXPECT_EQ(schedule.OutputDeadline(0), 0);
    EXPECT_EQ(schedule.OutputDeadline(3), 0);
    schedule.Clear();
    EXPECT_EQ(schedule.OutputDeadline(1), 0);
    schedule.Arm(1000, 1300, 1);
    EXPECT_EQ(schedule.OutputDeadline(1), 0);
}

TEST(FpsLimiterInterposerOutputSiteTest, APrivateChainPresentSelectsTheSite) {
    EXPECT_EQ(ResolveDxgiPresentSite(false, false, true), kOutputSite);
    EXPECT_EQ(ResolveDxgiPresentSite(false, true, true), kOutputSite) << "the private chain is a structural fact";
    EXPECT_EQ(ResolveDxgiPresentSite(true, false, true), PresentSite::kRuntimeOutputPresent);
    EXPECT_EQ(ResolveDxgiPresentSite(false, false), PresentSite::kUniqueApplicationPresent);
}

// The 2 ms duplicate window is the bug's other half: presents of the next frame escaped through it (176.8 outputs/s at a
// 140 cap, 20261106_181807), and the waited present moved between the outputs of a pair.
TEST(FpsLimiterInterposerOutputSiteTest, NoTimeWindowAndTheStrictGrid) {
    for (const bool fg : {false, true}) {
        EXPECT_TRUE(ShouldGateEveryApplyOnCadenceGrid(kOutputSite, fg));
        EXPECT_FALSE(ShouldUseDuplicatePresentWindow(kOutputSite, fg));
    }
}

TEST(FpsLimiterInterposerOutputSiteTest, GroupsOnTheExactRationalGrid) {
    auto plan = ResolveLocalCadencePlan(130, 43, true, 3, true, kOutputSite);
    EXPECT_EQ(plan.presentCadenceTargetFps, 130);
    EXPECT_EQ(plan.presentCadenceScale, 3);
    // Status not decided yet: every output its own slot at the target, which is the same grid.
    plan = ResolveLocalCadencePlan(140, 140, false, 1, true, kOutputSite);
    EXPECT_EQ(plan.presentCadenceTargetFps, 140);
    EXPECT_EQ(plan.presentCadenceScale, 1);
    // Base-frame capture sync keeps its target as the frame rate, unscaled.
    plan = ResolveLocalCadencePlan(60, 60, true, 2, false, kOutputSite);
    EXPECT_EQ(plan.presentCadenceTargetFps, 60);
    EXPECT_EQ(plan.presentCadenceScale, 1);
}

TEST(FpsLimiterInterposerOutputSiteTest, OneGameSleepCoversOneGroup) {
    EXPECT_EQ(ResolveEvaluationsPerGameFrame(kOutputSite, true, 2), 1u);
}

TEST(FpsLimiterInterposerOutputSiteTest, TheWrapperInFrontOfAnInterposerIsTheInputSite) {
    EXPECT_EQ(ResolveWrapperPresentSite(true), kInputSite);
    EXPECT_EQ(ResolveWrapperPresentSite(false), PresentSite::kUniqueApplicationPresent);
}

TEST(FpsLimiterInterposerOutputSiteTest, VulkanSelectsTheSiteForSmoothMotionOnly) {
    EXPECT_EQ(ResolveVulkanFinalOutputSite(RuntimeMode::kNvidiaSmoothMotion), kOutputSite);
    EXPECT_EQ(ResolveVulkanFinalOutputSite(RuntimeMode::kDLSSFG), PresentSite::kFinalOutputBoundary);
    EXPECT_EQ(ResolveVulkanFinalOutputSite(RuntimeMode::kOff), PresentSite::kFinalOutputBoundary);
}

// One of NvPresent's workers: runs Apply() when asked, on a thread that stays alive so its id stays its own.
class PresentWorker {
public:
    PresentWorker() : thread_([this] { Run(); }) {}
    ~PresentWorker() {
        Call(nullptr);
        thread_.join();
    }
    void Call(std::function<void()> work) {
        std::unique_lock<std::mutex> lock(mutex_);
        work_ = std::move(work);
        pending_ = true;
        wake_.notify_all();
        wake_.wait(lock, [this] { return !pending_; });
    }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return pending_; });
            const bool stop = !work_;
            if (work_) {
                work_();
            }
            pending_ = false;
            wake_.notify_all();
            if (stop) {
                return;
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::function<void()> work_;
    bool pending_ = false;
    std::thread thread_;
};

class FpsLimiterInterposerOutputApplyTest : public FpsLimiterTest {
protected:
    void ConfigureCap(int fps, int multiplier = 2) {
        UseVirtualClock();
        mockShm->fpsLimiter.SetGeneralEnabled(true);
        mockShm->fpsLimiter.SetGeneralFps(fps);
        mockShm->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
        if (multiplier > 1) {
            g_FGCompat.SetFSRFGMultiplier(multiplier);
            g_FGCompat.SetFSRFGActive(true);
        }
    }

    void TearDown() override {
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        FpsLimiterTest::TearDown();
    }

    // The moment the output reaches the real Present.
    int64_t Output() {
        limiter.Apply(true, kOutputSite);
        return clock.Now();
    }
};

// The gaps NvPresent left between the presents of a frame in 20261106_181807 and between frames, against one 140 fps
// grid: however the driver bunches them, every output is released one output interval after the last.
TEST_F(FpsLimiterInterposerOutputApplyTest, EveryOutputOfACappedFrameGetsItsOwnSlot) {
    ConfigureCap(140);
    const int64_t outputInterval = FirstIntervalTicks(140, 2) / 2;
    Output();  // arms the grid
    Output();

    const int64_t gapsUs[][2] = {{1650, 270}, {1600, 240}, {500, 1570}, {3100, 290}, {240, 1600}, {1900, 400}};
    std::vector<int64_t> releases;
    for (const auto& gaps : gapsUs) {
        clock.Advance(TicksFromUs(gaps[1]));
        releases.push_back(Output());
        clock.Advance(TicksFromUs(gaps[0]));
        releases.push_back(Output());
    }
    for (size_t index = 1; index < releases.size(); ++index) {
        EXPECT_NEAR(releases[index] - releases[index - 1], outputInterval, 1) << "output " << index;
    }
}

TEST_F(FpsLimiterInterposerOutputApplyTest, ThreeTimesDividesTheGroupIntoThirds) {
    ConfigureCap(180, 3);
    const int64_t outputInterval = FirstIntervalTicks(180, 3) / 3;
    for (int output = 0; output < 3; ++output) {
        Output();
    }
    std::vector<int64_t> releases;
    for (int output = 0; output < 9; ++output) {
        clock.Advance(TicksFromUs(100));
        releases.push_back(Output());
    }
    for (size_t index = 1; index < releases.size(); ++index) {
        EXPECT_NEAR(releases[index] - releases[index - 1], outputInterval, 1) << "output " << index;
    }
}

// A frame that arrives after its slot: the driver's own flip schedule spaces it (flat at 109 fps with no limiter), and
// CE's wait would only collapse that.
TEST_F(FpsLimiterInterposerOutputApplyTest, BelowTheCapTheDriverKeepsItsOwnSpacing) {
    ConfigureCap(140);
    const int64_t groupInterval = FirstIntervalTicks(140, 2);
    Output();
    Output();
    for (int frame = 0; frame < 4; ++frame) {
        clock.Advance(groupInterval + TicksFromUs(4000));
        EXPECT_EQ(VirtualTicksOf([&] { Output(); }), 0) << "frame " << frame;
        clock.Advance(TicksFromUs(300));
        EXPECT_EQ(VirtualTicksOf([&] { Output(); }), 0) << "frame " << frame;
    }
}

TEST_F(FpsLimiterInterposerOutputApplyTest, ALateFrameDoesNotInheritTheLastFramesSlot) {
    ConfigureCap(140);
    const int64_t groupInterval = FirstIntervalTicks(140, 2);
    Output();
    EXPECT_GT(VirtualTicksOf([&] { Output(); }), 0);
    clock.Advance(groupInterval * 2);
    EXPECT_EQ(VirtualTicksOf([&] { Output(); }), 0);
    EXPECT_EQ(VirtualTicksOf([&] { Output(); }), 0) << "the previous group's schedule was still armed";
}

// The second output is already later than its slot: no wait and no effect on the grid.
TEST_F(FpsLimiterInterposerOutputApplyTest, AnOutputPastItsSlotPassesAndTheGridHolds) {
    ConfigureCap(140);
    const int64_t groupInterval = FirstIntervalTicks(140, 2);
    Output();
    Output();
    const int64_t owner = Output();
    clock.Advance(groupInterval * 3 / 4);
    EXPECT_EQ(VirtualTicksOf([&] { Output(); }), 0);
    EXPECT_NEAR(Output() - owner, groupInterval, 1);
}

// Status not decided (the first half second, or generation not engaged): the same grid, one slot per output.
TEST_F(FpsLimiterInterposerOutputApplyTest, WithoutGenerationEveryOutputPacesAtTheTarget) {
    ConfigureCap(140, 1);
    Output();
    for (int output = 0; output < 4; ++output) {
        EXPECT_NEAR(VirtualTicksOf([&] { Output(); }), FirstIntervalTicks(140), 1);
    }
}

// Each worker opens its own group: a worker's single output cannot make the next worker's first output a later one.
TEST_F(FpsLimiterInterposerOutputApplyTest, AThreadChangeOpensAGroupOnItsSlot) {
    ConfigureCap(140);
    PresentWorker first;
    PresentWorker second;
    const int64_t groupInterval = FirstIntervalTicks(140, 2);
    int64_t ownerRelease = 0;
    first.Call([&] { Output(); });
    first.Call([&] { ownerRelease = Output(); });
    first.Call([&] { ownerRelease = Output(); });
    int64_t nextOwnerRelease = 0;
    second.Call([&] { nextOwnerRelease = Output(); });
    EXPECT_NEAR(nextOwnerRelease - ownerRelease, groupInterval, 1);
    int64_t follower = 0;
    second.Call([&] { follower = Output(); });
    EXPECT_NEAR(follower - nextOwnerRelease, groupInterval / 2, 1);
}

// DX12: the game thread owns the cadence and the outputs pass untouched, so the driver spaces them. They start pacing
// again if the application's Present stops reaching CE, and the wrapper takes it back with the next one.
TEST_F(FpsLimiterInterposerOutputApplyTest, TheApplicationsPresentOwnsPacingWhereCeSeesIt) {
    ConfigureCap(140);
    limiter.Apply(false, kInputSite);
    const uint32_t generation = limiter.GetResolvedCadence().generation;
    for (int output = 0; output < 2; ++output) {
        EXPECT_EQ(VirtualTicksOf([&] { Output(); }), 0);
    }
    EXPECT_EQ(limiter.GetRuntimeInputPassCount(), 2u);
    EXPECT_EQ(limiter.GetResolvedCadence().generation, generation);
    EXPECT_EQ(limiter.GetResolvedCadence().cadenceScale, 2);
    EXPECT_NEAR(VirtualTicksOf([&] { limiter.Apply(false, kInputSite); }), FirstIntervalTicks(140, 2), 1);

    const uint32_t generationAfterInput = limiter.GetResolvedCadence().generation;
    for (uint32_t output = 0; output < ce::runtime_input_site_policy::kOutputsBeforeOwnershipLapses; ++output) {
        Output();
    }
    EXPECT_EQ(limiter.GetResolvedCadence().generation, generationAfterInput);
    Output();
    EXPECT_GT(limiter.GetResolvedCadence().generation, generationAfterInput) << "the outputs must pace again";
}

}  // namespace
