#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "common/capture/capture_policy/recording_health.h"
#include "common/capture/encoder_iteration.h"
#include "source_fragment_reader.h"

namespace {

std::string ReadEncoderSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "captureengine" / "media" / name);
}

}  // namespace

TEST(EncoderFailureShutdownTest, TerminalFailureWinsOverNextIterationAndRetainedDebt) {
    constexpr std::array steps{0, 1, 2};
    ce::capture_policy::VideoOutputFailureStreak failure;
    std::vector<int> visited;
    bool nextIteration = false;
    constexpr uint64_t outputTicks = 13;
    constexpr uint64_t scheduledTicks = 641;
    for (uint64_t nowMs = 1000; nowMs <= 7000; nowMs += 1000) {
        const bool running = ce::encoder_iteration::RunSteps(steps, [&](int step) {
            visited.push_back(step);
            if (step == 1) {
                // A failed repeat leaves the same CFR slot outstanding and requests another pass.
                for (int attempt = 0; attempt < 2; ++attempt) {
                    ce::capture_policy::ObserveVideoOutputAttempt(failure, false, false, nowMs);
                }
                nextIteration = true;
            }
        }, [&] { return failure.stopRequested; }, [&] { return nextIteration; });
        nextIteration = false;
        if (failure.stopRequested) {
            EXPECT_FALSE(running);
            EXPECT_EQ(nowMs, 6000u);
            break;
        }
        EXPECT_TRUE(running);
    }
    EXPECT_TRUE(failure.stopRequested);
    EXPECT_LT(outputTicks, scheduledTicks);
    EXPECT_EQ(visited.size(), 12u);
    for (size_t i = 0; i < visited.size(); i += 2) {
        EXPECT_EQ(visited[i], 0);
        EXPECT_EQ(visited[i + 1], 1);
    }

    // The terminal latch belongs to one recording. A fresh worker can emit normally.
    ce::capture_policy::VideoOutputFailureStreak nextRecording;
    visited.clear();
    EXPECT_TRUE(ce::encoder_iteration::RunSteps(steps, [&](int step) {
        visited.push_back(step);
        ce::capture_policy::ObserveVideoOutputAttempt(nextRecording, true, false, 8000);
    }, [&] { return nextRecording.stopRequested; }, [] { return false; }));
    EXPECT_EQ(visited, (std::vector<int>{0, 1, 2}));
    EXPECT_FALSE(nextRecording.stopRequested);
}

TEST(EncoderFailureShutdownTest, DeferralsAndRecoveredFramesKeepTheHealthyWorkerRunning) {
    constexpr std::array steps{0, 1};
    ce::capture_policy::VideoOutputFailureStreak failure;
    int completedSteps = 0;
    for (uint64_t nowMs = 0; nowMs <= 20000; nowMs += 1000) {
        EXPECT_TRUE(ce::encoder_iteration::RunSteps(steps, [&](int step) {
            ++completedSteps;
            // Long backpressure remains retryable. Each real failure is followed by output.
            ce::capture_policy::ObserveVideoOutputAttempt(failure, step == 1, nowMs < 10000, nowMs);
        }, [&] { return failure.stopRequested; }, [] { return false; }));
    }
    EXPECT_EQ(completedSteps, 42);
    EXPECT_EQ(failure.failures, 0u);
    EXPECT_FALSE(failure.stopRequested);
}

TEST(EncoderFailureShutdownTest, ExplicitStopAlsoWinsOverPhaseRetry) {
    constexpr std::array steps{0, 1};
    int calls = 0;
    EXPECT_FALSE(ce::encoder_iteration::RunSteps(steps, [&](int) { ++calls; },
                                                [] { return true; }, [] { return true; }));
    EXPECT_EQ(calls, 1);
}

TEST(EncoderFailureShutdownTest, WorkerUsesTerminalOutputLatchBeforeRetryingAnyPhase) {
    const std::string source = ReadEncoderSource("media_main_encoder_00_session.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("ce::encoder_iteration::RunSteps(kSteps"), std::string::npos);
    EXPECT_NE(source.find("breakMainLoop || videoOutputFailureStreak.stopRequested"), std::string::npos);
    EXPECT_NE(source.find("if (!keepRunning)"), std::string::npos);
    EXPECT_NE(source.find("abandoning unencodable CFR debt"), std::string::npos);
}

TEST(EncoderFailureShutdownTest, EveryRepeatAndCatchupOutputFeedsTheFailureMonitor) {
    const std::string source = ReadEncoderSource("media_main_encoder_07_loop_emit.cpp");
    ASSERT_FALSE(source.empty());
    for (const char* observation : {
             "observeVideoOutputAttempt(blackSubmission, \"privacy VFR hold\")",
             "observeVideoOutputAttempt(submission, \"scheduled repeat\")",
             "observeVideoOutputAttempt(catchupSubmission, \"inject fresh catch-up\")",
             "observeVideoOutputAttempt(catchupSubmission, \"WGC fresh catch-up\")",
             "observeVideoOutputAttempt(repeatSubmission, \"catch-up repeat\")"}) {
        EXPECT_NE(source.find(observation), std::string::npos) << observation;
    }
}
