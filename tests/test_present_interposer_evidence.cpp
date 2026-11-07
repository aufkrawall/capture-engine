// Regression tests for Smooth Motion's status (hook/present/present_interposer_evidence.h, hook/fg/fg_interposer_status.cpp).
//
// Trine 5 DX11, session 20261106_171629: Smooth Motion generated 2x for the whole session, and the overlay said so for
// four flickering seconds and then never again. The present log shows 3452 presents in pairs on NvPresent's workers;
// every cadence window read application == output, because NvPresent's own device was CE-wrapped and its draws, and CE's
// overlay, counted as the game's. The runs below replay that stream.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "hook/fg/fg_detection.h"
#include "hook/present/present_interposer_evidence.h"
#include "hook/runtime/hook_clock.h"

namespace {

using namespace ce::present_interposer;
using ce::fg_runtime::RuntimeMode;

constexpr uint32_t kWorkers[] = {0x21E4, 0x33E0, 0x6068, 0x0AAC, 0x1B90, 0x4D88, 0x1884, 0x69B0};
constexpr int kWorkerCount = static_cast<int>(sizeof(kWorkers) / sizeof(kWorkers[0]));

struct JobShape {
    int64_t framePeriodUs;
    // The gap inside a job. The pair in the session's vsynced stretch was 10000/3900 us, uncapped 6000/500 us.
    int64_t innerGapUs;
    int outputs = 2;
};

// One job per application frame, each on the next worker. Returns the time after the last one.
template <typename Sink>
int64_t PlayJobs(const JobShape& shape, int frames, int64_t startUs, Sink&& sink) {
    int64_t nowUs = startUs;
    for (int frame = 0; frame < frames; ++frame) {
        const uint32_t worker = kWorkers[frame % kWorkerCount];
        for (int output = 0; output < shape.outputs; ++output) {
            sink(worker, nowUs + output * shape.innerGapUs);
        }
        nowUs += shape.framePeriodUs;
    }
    return nowUs;
}

std::vector<WindowReport> RunWindow(WorkerRunWindow& window, const JobShape& shape, int frames, int64_t startUs = 0) {
    std::vector<WindowReport> reports;
    PlayJobs(shape, frames, startUs, [&](uint32_t worker, int64_t nowUs) {
        const auto result = window.Note(worker, nowUs);
        if (result.windowClosed) {
            reports.push_back(result.report);
        }
    });
    return reports;
}

TEST(PresentInterposerWorkerRunsTest, TwoPresentsPerWorkerJobIsTwoXAtTheSessionsCadence) {
    WorkerRunWindow window;
    const auto reports = RunWindow(window, {13'889, 3'900}, 144);
    ASSERT_FALSE(reports.empty());
    const CadenceVerdict& verdict = reports.front().verdict;
    EXPECT_TRUE(verdict.decided);
    EXPECT_TRUE(verdict.generating);
    EXPECT_EQ(verdict.multiplier, 2);
    EXPECT_NEAR(verdict.applicationFps, 72.0f, 1.0f);
    EXPECT_NEAR(verdict.outputFps, 144.0f, 2.0f);
    EXPECT_EQ(reports.front().stream, EvidenceStream::kWorkerRuns);
    EXPECT_EQ(reports.front().modalRun, 2);
}

// The evidence is structural: no refresh rate, cap, GPU load or spacing inside the pair may change the verdict.
TEST(PresentInterposerWorkerRunsTest, TheVerdictDoesNotDependOnTheCadence) {
    const JobShape shapes[] = {
        {6'329, 500},     // 158 base fps uncapped, the session's start
        {13'889, 500},    // 72 x 2, pair presented together
        {13'889, 10'000}, // 72 x 2, pair spread by the driver's metering
        {10'250, 400},    // 97 x 2 with the panel far away
        {16'667, 8'333},  // 60 Hz panel, 120 output
        {33'333, 16'667}, // 30 base under heavy GPU load
        {4'167, 2'083},   // 240 output
        {2'778, 1'389},   // 360 base, no vsync
        {42'000, 21'000}, // 24 base
    };
    for (const JobShape& shape : shapes) {
        WorkerRunWindow window;
        const int frames = static_cast<int>(3'000'000 / shape.framePeriodUs);
        const auto reports = RunWindow(window, shape, frames);
        ASSERT_FALSE(reports.empty()) << shape.framePeriodUs;
        for (const WindowReport& report : reports) {
            EXPECT_TRUE(report.verdict.generating) << shape.framePeriodUs << "/" << shape.innerGapUs;
            EXPECT_EQ(report.verdict.multiplier, 2) << shape.framePeriodUs << "/" << shape.innerGapUs;
        }
    }
}

// A pool can hand one worker two jobs in a row (70 of 25711 runs): a run of four is two frames of 2x, not one of 4x.
TEST(PresentInterposerWorkerRunsTest, AWorkerThatGotTwoJobsInARowIsTwoFrames) {
    WorkerRunWindow window;
    std::vector<WindowReport> reports;
    int64_t nowUs = 0;
    for (int frame = 0; frame < 400; ++frame) {
        const uint32_t worker = frame % 10 == 1 ? kWorkers[(frame - 1) % kWorkerCount] : kWorkers[frame % kWorkerCount];
        for (int output = 0; output < 2; ++output) {
            const auto result = window.Note(worker, nowUs + output * 3'900);
            if (result.windowClosed) {
                reports.push_back(result.report);
            }
        }
        nowUs += 13'889;
    }
    ASSERT_GE(reports.size(), 2u);
    for (const WindowReport& report : reports) {
        EXPECT_EQ(report.verdict.multiplier, 2);
        EXPECT_EQ(report.modalRun, 2);
        EXPECT_NEAR(report.verdict.applicationFps, 72.0f, 1.0f);
        EXPECT_NEAR(report.verdict.outputFps, 144.0f, 2.0f);
    }
}

TEST(PresentInterposerWorkerRunsTest, OnePresentPerJobIsNotGenerating) {
    WorkerRunWindow window;
    const auto reports = RunWindow(window, {6'944, 0, 1}, 300);
    ASSERT_FALSE(reports.empty());
    for (const WindowReport& report : reports) {
        EXPECT_TRUE(report.verdict.decided);
        EXPECT_FALSE(report.verdict.generating);
        EXPECT_EQ(report.verdict.multiplier, 1);
        EXPECT_EQ(report.modalRun, 1);
    }
}

TEST(PresentInterposerWorkerRunsTest, ALargerFactorIsMeasuredNotAssumed) {
    WorkerRunWindow window;
    const auto reports = RunWindow(window, {20'000, 5'000, 3}, 100);
    ASSERT_FALSE(reports.empty());
    EXPECT_TRUE(reports.front().verdict.generating);
    EXPECT_EQ(reports.front().verdict.multiplier, 3);
    EXPECT_EQ(reports.front().modalRun, 3);
}

// A generated frame dropped now and then is still a generator: 2x with one lone present in five jobs.
TEST(PresentInterposerWorkerRunsTest, DroppedGeneratedFramesDoNotEndGeneration) {
    WorkerRunWindow window;
    std::vector<WindowReport> reports;
    int64_t nowUs = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const int outputs = frame % 5 == 0 ? 1 : 2;
        for (int output = 0; output < outputs; ++output) {
            const auto result = window.Note(kWorkers[frame % kWorkerCount], nowUs + output * 3'900);
            if (result.windowClosed) {
                reports.push_back(result.report);
            }
        }
        nowUs += 13'889;
    }
    ASSERT_FALSE(reports.empty());
    EXPECT_TRUE(reports.front().verdict.generating);
    EXPECT_EQ(reports.front().verdict.multiplier, 2);
}

// Below 20 fps a one-second window never decided, and that read as "not generating".
TEST(PresentInterposerWorkerRunsTest, ALowFrameRateDecidesFromALongerWindow) {
    {
        WorkerRunWindow window;
        const auto reports = RunWindow(window, {125'000, 62'500}, 40);  // 8 fps
        ASSERT_FALSE(reports.empty());
        EXPECT_TRUE(reports.front().verdict.decided);
        EXPECT_TRUE(reports.front().verdict.generating);
        EXPECT_NEAR(reports.front().verdict.applicationFps, 8.0f, 0.5f);
    }
    {
        WorkerRunWindow window;
        const auto reports = RunWindow(window, {400'000, 200'000}, 20);  // 2.5 fps
        ASSERT_FALSE(reports.empty());
        EXPECT_TRUE(reports.front().verdict.decided);
        EXPECT_TRUE(reports.front().verdict.generating);
    }
}

TEST(PresentInterposerWorkerRunsTest, TooFewFramesEvenInALongWindowDecideNothing) {
    WorkerRunWindow window;
    const auto reports = RunWindow(window, {1'300'000, 500'000}, 12);  // 0.77 fps: four frames in the long window
    ASSERT_FALSE(reports.empty());
    EXPECT_FALSE(reports.front().verdict.decided);
    EXPECT_FALSE(reports.front().verdict.generating);
}

// One thread presenting everything has no runs to tell frames apart, whatever it presents.
TEST(PresentInterposerWorkerRunsTest, ADedicatedPresenterSaysNothingUntilThePoolReturns) {
    WorkerRunWindow window;
    bool measuredAtTheEnd = true;
    int64_t nowUs = 0;
    for (int present = 0; present < 600; ++present) {
        const auto result = window.Note(0x1234, nowUs);
        EXPECT_FALSE(result.windowClosed);
        measuredAtTheEnd = result.present.measured;
        nowUs += 6'944;
    }
    EXPECT_FALSE(measuredAtTheEnd);

    const auto reports = RunWindow(window, {13'889, 3'900}, 150, nowUs);
    ASSERT_FALSE(reports.empty());
    EXPECT_TRUE(reports.front().verdict.generating);
    EXPECT_NEAR(reports.front().verdict.outputFps, 144.0f, 3.0f) << "the dedicated stretch is not part of the window";
}

// An alt-tab is no frame rate.
TEST(PresentInterposerWorkerRunsTest, AStallIsNotPartOfAWindow) {
    WorkerRunWindow window;
    auto reports = RunWindow(window, {13'889, 3'900}, 50);
    EXPECT_TRUE(reports.empty()) << "under a second";
    reports = RunWindow(window, {13'889, 3'900}, 100, 30'000'000);
    ASSERT_FALSE(reports.empty());
    EXPECT_NEAR(reports.front().verdict.outputFps, 144.0f, 3.0f);
    EXPECT_NEAR(reports.front().verdict.applicationFps, 72.0f, 2.0f);
}

TEST(PresentInterposerWorkerRunsTest, EachJobHasOneApplicationFrame) {
    WorkerRunWindow window;
    RunWindow(window, {13'889, 3'900}, 100);  // Learns the factor.
    int64_t nowUs = 2'000'000;
    const uint32_t worker = 0x5555;
    std::vector<bool> applicationFrames;
    for (int present = 0; present < 4; ++present) {
        const auto result = window.Note(worker, nowUs);
        EXPECT_TRUE(result.present.measured);
        applicationFrames.push_back(result.present.applicationFrame);
        nowUs += 3'900;
    }
    EXPECT_EQ(applicationFrames, (std::vector<bool>{true, false, true, false}));
}

TEST(PresentInterposerLatchTest, AFewNotGeneratingWindowsTakeToBelieveAndUndecidedOnesSayNothing) {
    CadenceVerdict generating;
    generating.decided = true;
    generating.generating = true;
    generating.multiplier = 2;
    CadenceVerdict notGenerating;
    notGenerating.decided = true;
    CadenceVerdict undecided;

    GenerationLatch latch;
    EXPECT_FALSE(latch.Observe(notGenerating));
    EXPECT_FALSE(latch.Observe(undecided));
    EXPECT_TRUE(latch.Observe(generating));
    for (int window = 0; window < 20; ++window) {
        EXPECT_TRUE(latch.Observe(undecided));
    }
    for (int window = 1; window < GenerationLatch::kNotGeneratingWindowsToClear; ++window) {
        EXPECT_TRUE(latch.Observe(notGenerating));
    }
    EXPECT_TRUE(latch.Observe(generating)) << "one generating window starts the count over";
    for (int window = 1; window < GenerationLatch::kNotGeneratingWindowsToClear; ++window) {
        EXPECT_TRUE(latch.Observe(notGenerating));
    }
    EXPECT_FALSE(latch.Observe(notGenerating));
}

// DX12 with the wrapper's view: the application's presents are exact, and the runs only cross-check.
TEST(PresentInterposerEvidenceTest, TheApplicationStreamOutranksTheRuns) {
    InterposerEvidence evidence;
    OutputPresentClass present;
    WindowReport report;
    std::vector<WindowReport> reports;
    int64_t nowUs = 0;
    for (int frame = 0; frame < 300; ++frame) {
        const uint32_t worker = kWorkers[frame % kWorkerCount];
        // Pairs on the workers, and an application stream that says one frame per present.
        for (int output = 0; output < 2; ++output) {
            if (evidence.NoteOutput(worker, nowUs + output * 3'900, &present, &report)) {
                reports.push_back(report);
            }
            if (evidence.NoteApplication(nowUs + output * 3'900, &report)) {
                reports.push_back(report);
            }
        }
        nowUs += 13'889;
    }
    ASSERT_FALSE(reports.empty());
    for (const WindowReport& window : reports) {
        EXPECT_EQ(window.stream, EvidenceStream::kApplicationPresents);
        EXPECT_FALSE(window.verdict.generating);
    }
    EXPECT_EQ(reports.back().modalRun, 2) << "the runs are still there to compare with";
}

TEST(PresentInterposerEvidenceTest, TheRunsTakeOverOnceTheApplicationStreamIsGone) {
    InterposerEvidence evidence;
    OutputPresentClass present;
    WindowReport report;
    (void)evidence.NoteApplication(0, &report);
    std::vector<WindowReport> reports;
    PlayJobs({13'889, 3'900}, 600, 100'000, [&](uint32_t worker, int64_t nowUs) {
        if (evidence.NoteOutput(worker, nowUs, &present, &report)) {
            reports.push_back(report);
        }
    });
    ASSERT_FALSE(reports.empty());
    EXPECT_EQ(reports.front().stream, EvidenceStream::kWorkerRuns);
    EXPECT_TRUE(reports.front().verdict.generating);
}

std::unique_ptr<FGCompatibility> MakeWithNvPresent() {
    auto fg = std::make_unique<FGCompatibility>();
    fg->MarkNvPresentLoaded();
    return fg;
}

TEST(SmoothMotionStatusTest, NothingIsClaimedWithoutTheInterposer) {
    FGCompatibility fg;
    const auto present = fg.NoteInterposerOutputPresent(kWorkers[0], 0);
    EXPECT_FALSE(present.measured);
    EXPECT_TRUE(present.applicationFrame);
}

// The session: detected within a window, then never lost.
TEST(SmoothMotionStatusTest, ThePairedStreamIsDetectedOnceAndStays) {
    auto fg = MakeWithNvPresent();
    bool detected = false;
    PlayJobs({13'889, 3'900}, 72 * 22, 0, [&](uint32_t worker, int64_t nowUs) {
        fg->NoteInterposerOutputPresent(worker, nowUs);
        const bool active = fg->GetRuntimeMode() == RuntimeMode::kNvidiaSmoothMotion;
        EXPECT_FALSE(detected && !active) << "lost at " << nowUs;
        detected = detected || active;
    });
    ASSERT_TRUE(detected);
    EXPECT_TRUE(fg->IsFGActive());
    EXPECT_EQ(fg->GetFGMultiplier(), 2);
    EXPECT_NEAR(fg->GetBaseFPS(), 72.0f, 1.0f);
    EXPECT_NEAR(fg->GetOutputFPS(), 144.0f, 2.0f);
}

TEST(SmoothMotionStatusTest, ALowFrameRateIsDetectedAndKept) {
    auto fg = MakeWithNvPresent();
    int64_t nowUs = PlayJobs({100'000, 50'000}, 40, 0, [&](uint32_t worker, int64_t timeUs) {
        fg->NoteInterposerOutputPresent(worker, timeUs);
    });
    ASSERT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    EXPECT_NEAR(fg->GetBaseFPS(), 10.0f, 1.0f);

    // Slower still: windows with too few frames say nothing, so the status stands.
    PlayJobs({1'300'000, 500'000}, 30, nowUs, [&](uint32_t worker, int64_t timeUs) {
        fg->NoteInterposerOutputPresent(worker, timeUs);
        EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    });
}

TEST(SmoothMotionStatusTest, OneToOneIsBelievedOnlyAfterAFewWindows) {
    auto fg = MakeWithNvPresent();
    const auto note = [&](uint32_t worker, int64_t nowUs) { fg->NoteInterposerOutputPresent(worker, nowUs); };
    int64_t nowUs = PlayJobs({13'889, 3'900}, 300, 0, note);
    ASSERT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);

    // A window and a bit of forwarding keep the status and the factor.
    nowUs = PlayJobs({6'944, 0, 1}, 200, nowUs, note);
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    EXPECT_EQ(fg->GetFGMultiplier(), 2);

    PlayJobs({6'944, 0, 1}, 800, nowUs, note);
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kOff);
    EXPECT_FALSE(fg->IsFGActive());
}

TEST(SmoothMotionStatusTest, TheGamesOwnSubmissionsAnswerForADedicatedPresenter) {
    auto fg = MakeWithNvPresent();
    fg->SetApplicationSubmissionCountingEnabled(true);
    int64_t nowUs = 0;
    for (int frame = 0; frame < 300; ++frame) {
        for (int draw = 0; draw < 100; ++draw) {
            fg->NoteApplicationSubmission();
        }
        fg->NoteInterposerOutputPresent(0x1234, nowUs);
        fg->NoteInterposerOutputPresent(0x1234, nowUs + 3'900);
        nowUs += 13'889;
    }
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    EXPECT_EQ(fg->GetFGMultiplier(), 2);
    EXPECT_NEAR(fg->GetBaseFPS(), 72.0f, 2.0f);
}

TEST(SmoothMotionStatusTest, ADedicatedPresenterWithNoOtherEvidenceClaimsNothing) {
    auto fg = MakeWithNvPresent();
    fg->SetApplicationSubmissionCountingEnabled(true);
    int64_t nowUs = 0;
    for (int present = 0; present < 1000; ++present) {
        fg->NoteInterposerOutputPresent(0x1234, nowUs);
        nowUs += 6'944;
    }
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kOff);
}

TEST(SmoothMotionStatusTest, ClearingStateResetsMeasuredRatesAndMultiplier) {
    auto fg = MakeWithNvPresent();
    fg->SetApplicationSubmissionCountingEnabled(true);
    int64_t nowUs = 0;
    for (int frame = 0; frame < 300; ++frame) {
        for (int draw = 0; draw < 100; ++draw) {
            fg->NoteApplicationSubmission();
        }
        fg->NoteInterposerOutputPresent(0x1234, nowUs);
        fg->NoteInterposerOutputPresent(0x1234, nowUs + 3'900);
        nowUs += 13'889;
    }
    ASSERT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    ASSERT_GT(fg->GetOutputFPS(), 0.0f);
    fg->ClearNvidiaSMState();
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kOff);
    EXPECT_EQ(fg->GetFGMultiplier(), 0);
    EXPECT_EQ(fg->GetOutputFPS(), 0.0f);
    EXPECT_EQ(fg->GetBaseFPS(), 0.0f);
}

// The exact stream decides directly, as it does in DX12.
TEST(SmoothMotionStatusTest, TheApplicationStreamDecidesWhereItExists) {
    auto fg = MakeWithNvPresent();
    int64_t nowUs = 0;
    for (int frame = 0; frame < 150; ++frame) {
        fg->NoteInterposerApplicationPresent(nowUs);
        fg->NoteInterposerOutputPresent(0x1234, nowUs);
        fg->NoteInterposerOutputPresent(0x1234, nowUs + 3'900);
        nowUs += 13'889;
    }
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    EXPECT_NEAR(fg->GetBaseFPS(), 72.0f, 1.0f);
    EXPECT_NEAR(fg->GetOutputFPS(), 144.0f, 2.0f);
}

TEST(SmoothMotionStatusTest, ARealRuntimeOutranksTheCadence) {
    auto fg = MakeWithNvPresent();
    fg->SetFSRFGActive(true);
    PlayJobs({13'889, 3'900}, 300, 0, [&](uint32_t worker, int64_t nowUs) {
        fg->NoteInterposerOutputPresent(worker, nowUs);
    });
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kFSRFG);
}

// After Streamline's FG goes off the status starts over, so one forwarding window cannot bring back the old latch.
TEST(SmoothMotionStatusTest, ClearingTheStateClearsTheLatch) {
    auto fg = MakeWithNvPresent();
    const auto note = [&](uint32_t worker, int64_t nowUs) { fg->NoteInterposerOutputPresent(worker, nowUs); };
    int64_t nowUs = PlayJobs({13'889, 3'900}, 200, 0, note);
    ASSERT_EQ(fg->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    fg->ClearNvidiaSMState();
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kOff);
    PlayJobs({6'944, 0, 1}, 300, nowUs, note);
    EXPECT_EQ(fg->GetRuntimeMode(), RuntimeMode::kOff);
}

class SmoothMotionEpochTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ce::hook_clock::EnableVirtual();
    }
    void TearDown() override {
        ce::hook_clock::Virtual().enabled.store(false, std::memory_order_release);
    }
};

// A decided window owns only its epoch. After clearing, a dedicated presenter without application/submission
// evidence needs the paired-gap fallback again, whether the old window said generating or forwarding.
TEST_P(SmoothMotionEpochTest, ClearingRestoresHeuristicsForADedicatedPresenter) {
    auto fresh = MakeWithNvPresent();
    auto cleared = MakeWithNvPresent();
    PlayJobs({16'000, 1'000, GetParam() ? 2 : 1}, 200, 0, [&](uint32_t worker, int64_t nowUs) {
        cleared->NoteInterposerOutputPresent(worker, nowUs);
    });
    ASSERT_EQ(cleared->IsNvidiaSmoothMotionActive(), GetParam());
    cleared->ClearNvidiaSMState();
    ASSERT_EQ(cleared->GetRuntimeMode(), RuntimeMode::kOff);

    int64_t nowUs = 10'000'000;
    for (int frame = 0; frame < 600; ++frame) {
        for (int output = 0; output < 2; ++output) {
            const int64_t gapUs = output == 0 ? 15'000 : 1'000;
            ce::hook_clock::AdvanceMicroseconds(gapUs);
            nowUs += gapUs;
            for (FGCompatibility* fg : {fresh.get(), cleared.get()}) {
                fg->RecordPresentForNvidiaSmoothMotion(fg->NoteInterposerOutputPresent(0x1234, nowUs));
            }
        }
    }
    ASSERT_EQ(fresh->GetRuntimeMode(), RuntimeMode::kNvidiaSmoothMotion);
    EXPECT_EQ(cleared->GetRuntimeMode(), fresh->GetRuntimeMode());
}

INSTANTIATE_TEST_SUITE_P(PreviousCadence, SmoothMotionEpochTest, ::testing::Bool());

}  // namespace
