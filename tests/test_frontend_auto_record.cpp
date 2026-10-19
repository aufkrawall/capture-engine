// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/auto_record.h"

#include <gtest/gtest.h>

// The --auto-record script as a state machine on a fake clock (llm-wiki/library/05): no sleeps, no
// runtime. The scenarios are the old per-iteration block of ControllerMain, replayed through the events
// the async API delivers.
namespace {

using ce::frontend::AutoRecordScript;
using Step = AutoRecordScript::Step;
constexpr uint32_t kForever = AutoRecordScript::kWaitForever;

constexpr uint32_t kDelayMs = 3000;
constexpr uint32_t kDurationMs = 10000;
constexpr uint64_t kReadyMs = 1000;
constexpr uint64_t kStartDue = kReadyMs + kDelayMs;
constexpr uint64_t kStopDue = kReadyMs + kDelayMs + kDurationMs;

AutoRecordScript ReadyScript(uint32_t delayMs = kDelayMs, uint32_t durationMs = kDurationMs,
                             uint64_t ready = kReadyMs) {
    AutoRecordScript script(delayMs, durationMs);
    script.OnRuntimeState(CE_RUNTIME_STARTING, 0);
    script.OnRuntimeState(CE_RUNTIME_READY, ready);
    return script;
}

// Runs the start of the script to "recording requested": toggle at kStartDue, completion, STARTING.
void StartRecording(AutoRecordScript& script, ce_request_t request = 10) {
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(request);
    script.OnCommandCompleted(request, CE_OK);
    script.OnRecordingState(CE_RECORDING_STARTING, 0);
}

TEST(FrontendAutoRecordTest, InactiveScriptNeverActs) {
    AutoRecordScript script;
    EXPECT_FALSE(script.Active());
    script.OnRuntimeState(CE_RUNTIME_READY, 5);
    EXPECT_EQ(script.Poll(1'000'000), Step::None);
    EXPECT_EQ(script.WaitMs(1'000'000), kForever);
    EXPECT_FALSE(script.OnRecordingState(CE_RECORDING_LIVE, 3)) << "nothing to give up";
}

TEST(FrontendAutoRecordTest, TheClockStartsAtReadyNotBefore) {
    AutoRecordScript script(kDelayMs, kDurationMs);
    EXPECT_TRUE(script.Active());
    EXPECT_EQ(script.Poll(10'000'000), Step::None) << "no recording is started before the runtime is READY";
    EXPECT_EQ(script.WaitMs(10'000'000), kForever);
    script.OnRuntimeState(CE_RUNTIME_STARTING, 50);
    EXPECT_EQ(script.WaitMs(60), kForever);

    script.OnRuntimeState(CE_RUNTIME_READY, kReadyMs);
    EXPECT_EQ(script.WaitMs(kReadyMs), kDelayMs);
    script.OnRuntimeState(CE_RUNTIME_READY, 999'999);  // a repeated READY must not restart the delay
    EXPECT_EQ(script.WaitMs(kReadyMs), kDelayMs);
}

TEST(FrontendAutoRecordTest, StartsExactlyWhenTheDelayHasElapsed) {
    AutoRecordScript script = ReadyScript();
    EXPECT_EQ(script.Poll(kStartDue - 1), Step::None);
    EXPECT_EQ(script.WaitMs(kStartDue - 1), 1u);
    EXPECT_EQ(script.WaitMs(kStartDue), 0u);
    EXPECT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
}

TEST(FrontendAutoRecordTest, FullRunStartsStopsAndThenRequestsShutdownOnce) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(7);
    EXPECT_EQ(script.Poll(kStartDue), Step::None) << "one toggle at a time";
    EXPECT_EQ(script.WaitMs(kStartDue), kForever) << "only the completion can move it on";

    script.OnCommandCompleted(7, CE_OK);
    EXPECT_EQ(script.Poll(kStartDue), Step::None) << "waits for the state the toggle produced";
    script.OnRecordingState(CE_RECORDING_STARTING, 0);
    EXPECT_EQ(script.WaitMs(kStartDue + 100), kStopDue - (kStartDue + 100)) << "the stop is clocked from READY";
    script.OnRecordingState(CE_RECORDING_LIVE, 0);

    EXPECT_EQ(script.Poll(kStopDue - 1), Step::None);
    ASSERT_EQ(script.Poll(kStopDue), Step::ToggleRecording);
    script.OnToggleIssued(8);
    EXPECT_EQ(script.Poll(kStopDue + 5000), Step::None) << "shutdown follows the stop's completion, not a timer";
    EXPECT_EQ(script.WaitMs(kStopDue), kForever);
    script.OnRecordingState(CE_RECORDING_STOPPING, 0);
    EXPECT_EQ(script.Poll(kStopDue + 5000), Step::None);

    script.OnCommandCompleted(8, CE_OK);
    EXPECT_EQ(script.WaitMs(kStopDue), 0u);
    EXPECT_EQ(script.Poll(kStopDue + 5000), Step::RequestShutdown);
    EXPECT_FALSE(script.Active());
    EXPECT_EQ(script.Poll(kStopDue + 6000), Step::None) << "the shutdown is requested once";
    EXPECT_EQ(script.WaitMs(kStopDue + 6000), kForever);
}

TEST(FrontendAutoRecordTest, ACompletionThatOvertakesItsStateEventDoesNotToggleTwice) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(1);
    script.OnCommandCompleted(1, CE_OK);  // COMMAND_COMPLETED first, RECORDING_STATE not delivered yet
    EXPECT_EQ(script.Poll(kStartDue + 50), Step::None) << "a second toggle would stop the recording it started";
    EXPECT_EQ(script.WaitMs(kStartDue + 50), kForever);
    script.OnRecordingState(CE_RECORDING_STARTING, 0);
    EXPECT_EQ(script.Poll(kStartDue + 50), Step::None);
    EXPECT_EQ(script.WaitMs(kStartDue + 50), kStopDue - (kStartDue + 50));
}

TEST(FrontendAutoRecordTest, AStateEventThatOvertakesItsCompletionIsAlsoFine) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(1);
    script.OnRecordingState(CE_RECORDING_STARTING, 0);  // STARTING before COMMAND_COMPLETED
    EXPECT_EQ(script.Poll(kStartDue + 1), Step::None) << "still in flight";
    script.OnCommandCompleted(1, CE_OK);
    EXPECT_EQ(script.Poll(kStartDue + 1), Step::None);
    EXPECT_EQ(script.WaitMs(kStartDue + 1), kStopDue - (kStartDue + 1)) << "no extra event is needed";
}

TEST(FrontendAutoRecordTest, ARecordingThatFailsWithoutAFailureCodeIsStartedAgainAtOnce) {
    AutoRecordScript script = ReadyScript();
    StartRecording(script);
    EXPECT_FALSE(script.OnRecordingState(CE_RECORDING_IDLE, 0)) << "readiness failures stay retryable";
    EXPECT_EQ(script.WaitMs(kStartDue + 20), 0u);
    ASSERT_EQ(script.Poll(kStartDue + 20), Step::ToggleRecording);
    script.OnToggleIssued(11);
    script.OnCommandCompleted(11, CE_OK);
    script.OnRecordingState(CE_RECORDING_STARTING, 0);
    script.OnRecordingState(CE_RECORDING_LIVE, 0);
    EXPECT_EQ(script.Poll(kStopDue), Step::ToggleRecording) << "the stop deadline did not move";
}

TEST(FrontendAutoRecordTest, AnIntegrityFailureEndsTheScriptWithoutShuttingDown) {
    AutoRecordScript script = ReadyScript();
    StartRecording(script);
    script.OnRecordingState(CE_RECORDING_LIVE, 0);
    EXPECT_TRUE(script.OnRecordingState(CE_RECORDING_IDLE, 2)) << "reported exactly once, for the log line";
    EXPECT_FALSE(script.OnRecordingState(CE_RECORDING_IDLE, 2));
    EXPECT_FALSE(script.Active());
    EXPECT_EQ(script.Poll(kStopDue + 100'000), Step::None) << "no restart and no shutdown";
    EXPECT_EQ(script.WaitMs(kStopDue + 100'000), kForever);
}

TEST(FrontendAutoRecordTest, AFailureWhileTheStartIsStillInFlightEndsTheScriptToo) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(3);
    EXPECT_TRUE(script.OnRecordingState(CE_RECORDING_IDLE, 3));
    script.OnCommandCompleted(3, CE_E_PROCESS);
    EXPECT_EQ(script.Poll(kStopDue), Step::None);
}

TEST(FrontendAutoRecordTest, ARejectedToggleWaitsForNewInformationInsteadOfSpinning) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleRejected();  // e.g. CE_E_NO_MEMORY at admission
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(script.Poll(kStartDue + i), Step::None);
        EXPECT_EQ(script.WaitMs(kStartDue + i), kForever);
    }
    script.OnRecordingState(CE_RECORDING_IDLE, 0);
    EXPECT_EQ(script.Poll(kStartDue + 10), Step::ToggleRecording);
}

TEST(FrontendAutoRecordTest, AToggleThatCompletesWithAnErrorAndNoEventDoesNotSpinEither) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(5);
    script.OnCommandCompleted(5, CE_E_PROCESS);
    EXPECT_EQ(script.Poll(kStartDue + 1), Step::None);
    EXPECT_EQ(script.WaitMs(kStartDue + 1), kForever);
    script.OnRecordingState(CE_RECORDING_IDLE, 0);
    EXPECT_EQ(script.Poll(kStartDue + 1), Step::ToggleRecording);
}

TEST(FrontendAutoRecordTest, CompletionsOfOtherRequestsAreIgnored) {
    AutoRecordScript script = ReadyScript();
    ASSERT_EQ(script.Poll(kStartDue), Step::ToggleRecording);
    script.OnToggleIssued(5);
    script.OnCommandCompleted(4, CE_OK);
    script.OnCommandCompleted(6, CE_OK);
    script.OnRecordingState(CE_RECORDING_STARTING, 0);
    EXPECT_EQ(script.WaitMs(kStartDue + 1), kForever) << "request 5 is still outstanding";
    script.OnCommandCompleted(5, CE_OK);
    EXPECT_EQ(script.WaitMs(kStartDue + 1), kStopDue - (kStartDue + 1));
}

TEST(FrontendAutoRecordTest, ARecordingStartedByHandBeforeTheDelayIsOnlyStopped) {
    AutoRecordScript script = ReadyScript();
    script.OnRecordingState(CE_RECORDING_STARTING, 0);  // the user pressed the hotkey
    script.OnRecordingState(CE_RECORDING_LIVE, 0);
    EXPECT_EQ(script.Poll(kStartDue), Step::None) << "requested: nothing to start";
    EXPECT_EQ(script.WaitMs(kStartDue), kStopDue - kStartDue);
    EXPECT_EQ(script.Poll(kStopDue), Step::ToggleRecording);
}

TEST(FrontendAutoRecordTest, AManualStopMidRunStartsTheRecordingAgain) {
    AutoRecordScript script = ReadyScript();
    StartRecording(script);
    script.OnRecordingState(CE_RECORDING_LIVE, 0);
    script.OnRecordingState(CE_RECORDING_STOPPING, 0);  // not requested any more
    script.OnRecordingState(CE_RECORDING_IDLE, 0);
    EXPECT_EQ(script.Poll(kStartDue + 4000), Step::ToggleRecording) << "today's check keys off 'not requested'";
}

TEST(FrontendAutoRecordTest, StopRejectedSynchronouslyStillEndsWithShutdown) {
    AutoRecordScript script = ReadyScript();
    StartRecording(script);
    ASSERT_EQ(script.Poll(kStopDue), Step::ToggleRecording);
    script.OnToggleRejected();
    EXPECT_EQ(script.WaitMs(kStopDue), 0u);
    EXPECT_EQ(script.Poll(kStopDue), Step::RequestShutdown) << "the old loop left right after the stop attempt";
}

TEST(FrontendAutoRecordTest, StopCompletedWithAnErrorStillEndsWithShutdown) {
    AutoRecordScript script = ReadyScript();
    StartRecording(script);
    ASSERT_EQ(script.Poll(kStopDue), Step::ToggleRecording);
    script.OnToggleIssued(20);
    script.OnCommandCompleted(20, CE_E_INVALID_STATE);
    EXPECT_EQ(script.Poll(kStopDue), Step::RequestShutdown);
}

TEST(FrontendAutoRecordTest, TheRuntimeGoingAwayEndsTheScript) {
    for (int32_t state : {CE_RUNTIME_STOPPING, CE_RUNTIME_STOPPED, CE_RUNTIME_FAILED}) {
        AutoRecordScript script = ReadyScript();
        script.OnRuntimeState(state, kReadyMs + 10);
        EXPECT_FALSE(script.Active());
        EXPECT_EQ(script.Poll(kStopDue), Step::None);
        EXPECT_EQ(script.WaitMs(kStopDue), kForever);
    }
    AutoRecordScript failedStartup(kDelayMs, kDurationMs);
    failedStartup.OnRuntimeState(CE_RUNTIME_FAILED, 5);
    failedStartup.OnRuntimeState(CE_RUNTIME_READY, 6);
    EXPECT_EQ(failedStartup.Poll(100'000), Step::None) << "a failed runtime is never READY again";
}

TEST(FrontendAutoRecordTest, ZeroDelayAndZeroDurationRunBackToBack) {
    AutoRecordScript script = ReadyScript(0, 0, 500);
    EXPECT_EQ(script.WaitMs(500), 0u);
    ASSERT_EQ(script.Poll(500), Step::ToggleRecording);
    script.OnToggleIssued(1);
    script.OnCommandCompleted(1, CE_OK);
    script.OnRecordingState(CE_RECORDING_STARTING, 0);
    EXPECT_EQ(script.Poll(500), Step::ToggleRecording) << "duration 0: the stop is due as soon as it started";
}

TEST(FrontendAutoRecordTest, TimesNeverWrapAndWaitsAreClamped) {
    constexpr uint32_t kMax = 4294967295u;
    AutoRecordScript script = ReadyScript(kMax, kMax, 0);
    EXPECT_EQ(script.WaitMs(0), kMax - 1) << "a wait of kMax would read as 'forever'";
    EXPECT_EQ(script.Poll(uint64_t{kMax} - 1), Step::None);
    ASSERT_EQ(script.Poll(kMax), Step::ToggleRecording);
    script.OnToggleIssued(1);
    script.OnCommandCompleted(1, CE_OK);
    script.OnRecordingState(CE_RECORDING_LIVE, 0);
    EXPECT_EQ(script.WaitMs(kMax), kMax - 1) << "the stop is 2*kMax after READY, not a wrapped 32-bit sum";
    EXPECT_EQ(script.Poll(uint64_t{kMax} * 2 - 1), Step::None);
    EXPECT_EQ(script.Poll(uint64_t{kMax} * 2), Step::ToggleRecording);
}

TEST(FrontendAutoRecordTest, AClockBehindReadyIsNotAnUnderflow) {
    AutoRecordScript script = ReadyScript();
    EXPECT_EQ(script.Poll(kReadyMs - 1), Step::None);
    EXPECT_EQ(script.WaitMs(kReadyMs - 1), kDelayMs);
}

}  // namespace
