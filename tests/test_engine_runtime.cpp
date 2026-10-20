// Behavior of the runtime engine through its interface (llm-wiki/library/04 "runtime/core", 07): a real
// engine thread over fake helper children, fixed settings and a fake screenshot capture. Nothing spawns,
// registers hotkeys or opens windows. Waits are on runtime events with generous bounds, never sleeps.
#include <gtest/gtest.h>

#include "engine_runtime_harness.h"

namespace {
using namespace engine_runtime_test;

TEST(EngineRuntimeTest, StartsHelpersReachesReadyAndStopsInOrder) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    EXPECT_EQ(runtime->Status().runtimeState, CE_RUNTIME_READY);
    EXPECT_TRUE(runtime->Status().helpersReady & CE_HELPER_INJECT);
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
    const auto calls = harness.children->Calls();
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.front(), "ensure-inject");
    EXPECT_EQ(calls.back(), "shutdown");
}

TEST(EngineRuntimeTest, InjectSpawnFailureFailsAndShutsTheHelpersDown) {
    Harness harness;
    harness.children->injectStarts = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_FAILED); }, &seen));
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
    EXPECT_TRUE(harness.children->Called("shutdown"));
    for (const SeenEvent& event : seen)
        EXPECT_FALSE(event.type == CE_EVENT_RUNTIME_STATE && event.state == CE_RUNTIME_READY);
    EXPECT_EQ(runtime->Submit(Make(CommandKind::ToggleOverlay)).status, CE_E_INVALID_STATE);
}

TEST(EngineRuntimeTest, CommandsBeforeReadyAreNotAdmitted) {
    Harness harness;
    harness.children->injectGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    const Admission early = runtime->Submit(Make(CommandKind::ToggleOverlay));
    EXPECT_EQ(early.status, CE_E_INVALID_STATE);
    EXPECT_EQ(early.request, 0u);
    SetEvent(harness.children->injectGate);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    EXPECT_EQ(runtime->Submit(Make(CommandKind::ToggleOverlay)).status, CE_OK);
    runtime.reset();
    CloseHandle(harness.children->injectGate);
}

TEST(EngineRuntimeTest, EveryAdmittedCommandCompletesExactlyOnce) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    std::map<uint64_t, int> completions;
    std::vector<uint64_t> requests;
    for (CommandKind kind : {CommandKind::ToggleOverlay, CommandKind::ToggleBenchmark, CommandKind::TakeScreenshot,
                             CommandKind::StartRecording, CommandKind::StopRecording}) {
        const Admission admission = runtime->Submit(Make(kind));
        ASSERT_EQ(admission.status, CE_OK);
        requests.push_back(admission.request);
    }
    std::vector<SeenEvent> seen;
    size_t completed = 0;
    while (completed < requests.size()) {
        auto event = WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_COMMAND_COMPLETED; }, &seen);
        ASSERT_TRUE(event);
        ++completions[event->request];
        ++completed;
    }
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }, &seen));
    for (uint64_t request : requests)
        EXPECT_EQ(completions[request], 1) << "request " << request;
    for (const SeenEvent& event : seen) {
        if (event.type == CE_EVENT_COMMAND_COMPLETED)
            EXPECT_EQ(completions.count(event.request), 1u);
        if (event.type == CE_EVENT_SCREENSHOT) {
            EXPECT_EQ(event.request, requests[2]);
            EXPECT_EQ(event.status, CE_OK);
            EXPECT_EQ(event.pathCount, 2u);
        }
    }
    EXPECT_EQ(harness.captures.load(), 1);
}

TEST(EngineRuntimeTest, CommandsQueuedAtShutdownCompleteWithInvalidState) {
    Harness harness;
    harness.captureGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const Admission blocking = runtime->Submit(Make(CommandKind::TakeScreenshot));
    ASSERT_EQ(blocking.status, CE_OK);
    // The engine is now inside the capture; these wait in the queue.
    std::vector<uint64_t> queued;
    for (int i = 0; i < 3; ++i) {
        const Admission admission = runtime->Submit(Make(CommandKind::ToggleOverlay));
        ASSERT_EQ(admission.status, CE_OK);
        queued.push_back(admission.request);
    }
    runtime->RequestShutdown();
    EXPECT_EQ(runtime->Submit(Make(CommandKind::ToggleOverlay)).status, CE_E_INVALID_STATE);
    SetEvent(harness.captureGate);
    std::map<uint64_t, ce_status_t> outcomes;
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }, &seen));
    for (const SeenEvent& event : seen)
        if (event.type == CE_EVENT_COMMAND_COMPLETED) {
            EXPECT_EQ(outcomes.count(event.request), 0u) << "duplicate completion for " << event.request;
            outcomes[event.request] = event.status;
        }
    EXPECT_EQ(outcomes.size(), 4u);
    for (uint64_t request : queued)
        EXPECT_EQ(outcomes[request], CE_E_INVALID_STATE);
    runtime.reset();
    CloseHandle(harness.captureGate);
}

TEST(EngineRuntimeTest, RecordingStatesFollowTheSessionAndLiveObservation) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    ASSERT_EQ(runtime->Submit(Make(CommandKind::StartRecording, CE_MODE_AUDIO_ONLY)).status, CE_OK);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_STARTING &&
               e.data.recording.mode == CE_MODE_AUDIO_ONLY;
    }));
    harness.children->live = true;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_LIVE;
    }));
    EXPECT_EQ(runtime->Status().recordingState, CE_RECORDING_LIVE);
    ASSERT_EQ(runtime->Submit(Make(CommandKind::StopRecording)).status, CE_OK);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_STOPPING;
    }));
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_IDLE;
    }));
    EXPECT_TRUE(harness.children->Called("retire-media"));
}

TEST(EngineRuntimeTest, MediaReadinessFailureEndsTheStartWithAFailure) {
    Harness harness;
    harness.children->mediaStarts = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const Admission start = runtime->Submit(Make(CommandKind::StartRecording));
    ASSERT_EQ(start.status, CE_OK);
    std::vector<SeenEvent> seen;
    auto completion = WaitFor(*runtime, [&](const ce_event_t& e) {
        return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == start.request;
    }, &seen);
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->status, CE_E_PROCESS);
    // The failed state is published before the command's completion.
    const auto idle = std::find_if(seen.begin(), seen.end(), [](const SeenEvent& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.state == CE_RECORDING_IDLE && e.failure != 0;
    });
    ASSERT_NE(idle, seen.end());
    EXPECT_EQ(idle->failure, kRecordingFailureGeneric);
}

TEST(EngineRuntimeTest, PublishedSettingsFanOutInOrderAndAnnounceARevision) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    {
        std::lock_guard<std::mutex> lock(harness.settings->mutex);
        harness.settings->previousToPublish = harness.settings->config;
    }
    auto changed = WaitFor(*runtime, [](const ce_event_t& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; });
    ASSERT_TRUE(changed);
    EXPECT_EQ(runtime->Status().settingsRevision, 1u);
    const auto calls = harness.children->Calls();
    const auto reconfigure = std::find(calls.begin(), calls.end(), "reconfigure");
    ASSERT_NE(reconfigure, calls.end());
    EXPECT_NE(std::find(reconfigure, calls.end(),
                        "send-all-" + std::to_string(static_cast<int>(ProcessCommand::ReloadConfig))),
              calls.end())
        << "helpers are reconfigured before they are told to reload";
}

TEST(EngineRuntimeTest, WithoutWatchSettingsAnExternalEditIsNotApplied) {
    Harness harness;
    auto runtime = harness.Start(0);
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    {
        std::lock_guard<std::mutex> lock(harness.settings->mutex);
        harness.settings->previousToPublish = harness.settings->config;
    }
    // Admitted after the publish, so the loop iteration that completes it also services the settings.
    Command toggle;
    toggle.kind = CommandKind::ToggleOverlay;
    const Admission admission = runtime->Submit(toggle);
    ASSERT_EQ(admission.status, CE_OK);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(
        *runtime, [&](const ce_event_t& e) { return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == admission.request; },
        &seen));
    EXPECT_EQ(std::count_if(seen.begin(), seen.end(), [](const SeenEvent& e) { return e.type == CE_EVENT_SETTINGS_CHANGED; }),
              0);
    EXPECT_EQ(runtime->Status().settingsRevision, 0u);
    const auto calls = harness.children->Calls();
    EXPECT_EQ(std::find(calls.begin(), calls.end(), "reconfigure"), calls.end());
}

TEST(EngineRuntimeTest, AFailedHelperShutdownStillStopsTheEngine) {
    Harness harness;
    harness.children->shutdownSucceeds = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    runtime->RequestShutdown();
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
}

TEST(EngineRuntimeTest, StoppedIsPublishedBeforeTheShutdownIsAnnounced) {
    // ce_runtime_destroy accepts only a STOPPED (or FAILED) runtime whose thread finished. A client that saw the shutdown
    // announced must therefore find the state already published, or its destroy is refused and the handle stays live.
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    runtime->RequestShutdown();
    ASSERT_TRUE(runtime->WaitStopped(kWaitMs));
    EXPECT_EQ(runtime->Status().runtimeState, CE_RUNTIME_STOPPED);
}

TEST(EngineRuntimeTest, ARuntimeCanBeCreatedAgainAfterTheFirstIsDestroyed) {
    for (int round = 0; round < 2; ++round) {
        Harness harness;
        auto runtime = harness.Start();
        ASSERT_NE(runtime, nullptr);
        ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
        runtime.reset();
        EXPECT_TRUE(harness.children->Called("shutdown")) << "round " << round;
    }
}

TEST(EngineRuntimeTest, InvalidCommandsAreRejectedAtAdmission) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    EXPECT_EQ(runtime->Submit(Make(CommandKind::StartRecording, 7)).status, CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(runtime->Submit(Make(CommandKind::LaunchTarget)).status, CE_E_INVALID_ARGUMENT);
}

TEST(EngineRuntimeTest, AFailedLoggerStartIsAnnouncedAsLostBeforeReadyAndDoesNotStopTheRuntime) {
    Harness harness;
    harness.settings->logLevel = LogLevel::Info;  // a logger is wanted
    harness.children->loggerStarts = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }, &seen));
    EXPECT_EQ(HelperStates(seen, CE_HELPER_LOGGER), std::vector<int32_t>{CE_HELPER_STATE_LOST});
}

TEST(EngineRuntimeTest, InjectLossAndRecoveryAreEachAnnouncedOnce) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime,
                        [](const ce_event_t& e) { return IsHelper(e, CE_HELPER_INJECT, CE_HELPER_STATE_READY); },
                        &seen));
    SetHelperDown(harness, HostChild::Inject, true);
    ASSERT_TRUE(WaitFor(*runtime,
                        [](const ce_event_t& e) { return IsHelper(e, CE_HELPER_INJECT, CE_HELPER_STATE_LOST); },
                        &seen));
    SetHelperDown(harness, HostChild::Inject, false);
    ASSERT_TRUE(WaitFor(*runtime,
                        [](const ce_event_t& e) { return IsHelper(e, CE_HELPER_INJECT, CE_HELPER_STATE_RECOVERED); },
                        &seen));
    EXPECT_EQ(HelperStates(seen, CE_HELPER_INJECT),
              (std::vector<int32_t>{CE_HELPER_STATE_READY, CE_HELPER_STATE_LOST, CE_HELPER_STATE_RECOVERED}));
}

TEST(EngineRuntimeTest, AMediaChildThatCrashesWhileRecordingIsLostAndTheNextRecordingBringsItBack) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    ASSERT_EQ(runtime->Submit(Make(CommandKind::StartRecording)).status, CE_OK);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime,
                        [](const ce_event_t& e) { return IsHelper(e, CE_HELPER_MEDIA, CE_HELPER_STATE_READY); },
                        &seen));
    SetHelperDown(harness, HostChild::Media, true);
    ASSERT_TRUE(WaitFor(*runtime,
                        [](const ce_event_t& e) { return IsHelper(e, CE_HELPER_MEDIA, CE_HELPER_STATE_LOST); },
                        &seen));
    // The lost media child fails the recording, and the failure retires it: the helper is no longer wanted.
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_IDLE &&
               e.data.recording.failure != 0;
    }, &seen));
    SetHelperDown(harness, HostChild::Media, false);
    const Admission restart = runtime->Submit(Make(CommandKind::StartRecording));
    ASSERT_EQ(restart.status, CE_OK);
    const auto completion = WaitFor(*runtime, [&](const ce_event_t& e) {
        return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == restart.request;
    }, &seen);
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->status, CE_OK);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) {
        return IsHelper(e, CE_HELPER_MEDIA, CE_HELPER_STATE_READY) ||
               IsHelper(e, CE_HELPER_MEDIA, CE_HELPER_STATE_RECOVERED);
    }, &seen));
    // READY when an engine tick saw the child retired by the failure before the restart, RECOVERED when the restart
    // came first (the same episode). Both are correct; the sequence must not contain anything else.
    const std::vector<int32_t> states = HelperStates(seen, CE_HELPER_MEDIA);
    ASSERT_EQ(states.size(), 3u);
    EXPECT_EQ(states[0], CE_HELPER_STATE_READY);
    EXPECT_EQ(states[1], CE_HELPER_STATE_LOST);
    EXPECT_TRUE(states[2] == CE_HELPER_STATE_READY || states[2] == CE_HELPER_STATE_RECOVERED);
}

TEST(EngineRuntimeTest, AStoppedRecordingIsFinalizedWithItsRecordingIdAndPath) {
    Harness harness;
    harness.children->live = true;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t id = StartAndWaitLive(*runtime);
    ASSERT_NE(id, 0u);
    StopAndWaitIdle(*runtime);
    {
        std::lock_guard<std::mutex> lock(harness.children->mutex);
        EXPECT_EQ(harness.children->retired, std::vector<uint64_t>{id});
        EXPECT_EQ(harness.children->finalizing, 1u);
    }

    harness.children->Finish(Finalized(id, ce::media_result::Status::kSaved, "D:\\Captures\\capture_1.mkv"));
    std::map<uint64_t, FinalizedSeen> seen;
    CollectFinalized(*runtime, 1, seen);
    ASSERT_EQ(seen.count(id), 1u);
    EXPECT_EQ(seen[id].status, CE_OK);
    EXPECT_EQ(seen[id].path, "D:\\Captures\\capture_1.mkv");
}

TEST(EngineRuntimeTest, OverlappingRecordingsEachFinalizeUnderTheirOwnRecordingId) {
    Harness harness;
    harness.children->live = true;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t first = StartAndWaitLive(*runtime);
    ASSERT_NE(first, 0u);
    StopAndWaitIdle(*runtime);
    // The next recording starts while the first one's media is still finalizing (an immediate restart).
    const uint64_t second = StartAndWaitLive(*runtime);
    ASSERT_NE(second, 0u);
    ASSERT_NE(first, second);
    StopAndWaitIdle(*runtime);

    // The older media exits after the newer recording already exists: it must still name its own recording.
    harness.children->Finish(Finalized(first, ce::media_result::Status::kSaved, "D:\\Captures\\capture_a.mkv"));
    harness.children->Finish(Finalized(second, ce::media_result::Status::kSaved, "D:\\Captures\\capture_b.mkv"));
    std::map<uint64_t, FinalizedSeen> seen;
    CollectFinalized(*runtime, 2, seen);
    ASSERT_EQ(seen.count(first), 1u);
    ASSERT_EQ(seen.count(second), 1u);
    EXPECT_EQ(seen[first].path, "D:\\Captures\\capture_a.mkv");
    EXPECT_EQ(seen[second].path, "D:\\Captures\\capture_b.mkv");
}

TEST(EngineRuntimeTest, FinalizationOutcomesMapToTheEventStatusAndPath) {
    Harness harness;
    harness.children->live = true;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const uint64_t canceled = StartAndWaitLive(*runtime);
    StopAndWaitIdle(*runtime);
    harness.children->Finish(Finalized(canceled, ce::media_result::Status::kCanceled));
    std::map<uint64_t, FinalizedSeen> seen;
    CollectFinalized(*runtime, 1, seen);
    EXPECT_EQ(seen[canceled].status, CE_OK);
    EXPECT_FALSE(seen[canceled].hasPath);
    EXPECT_EQ(seen[canceled].message, "canceled");

    const uint64_t failed = StartAndWaitLive(*runtime);
    StopAndWaitIdle(*runtime);
    harness.children->Finish(Finalized(failed, ce::media_result::Status::kFailed));
    CollectFinalized(*runtime, 1, seen);
    EXPECT_EQ(seen[failed].status, CE_E_IO);
    EXPECT_FALSE(seen[failed].hasPath);

    const uint64_t killed = StartAndWaitLive(*runtime);
    StopAndWaitIdle(*runtime);
    harness.children->Finish(Killed(killed));
    CollectFinalized(*runtime, 1, seen);
    EXPECT_EQ(seen[killed].status, CE_E_PROCESS) << "a child that ended without a result is lost";
    EXPECT_FALSE(seen[killed].hasPath);

    const uint64_t nothing = StartAndWaitLive(*runtime);
    StopAndWaitIdle(*runtime);
    harness.children->Finish(Finalized(nothing, ce::media_result::Status::kNone));
    CollectFinalized(*runtime, 1, seen);
    EXPECT_EQ(seen[nothing].status, CE_OK);
    EXPECT_EQ(seen[nothing].message, "nothing recorded");
    EXPECT_FALSE(seen[nothing].hasPath);
}

TEST(EngineRuntimeTest, ARecordingStillFinalizingIsCountedInTheStatusUntilShutdown) {
    Harness harness;
    harness.children->live = true;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    StartAndWaitLive(*runtime);
    StopAndWaitIdle(*runtime);
    runtime->RequestShutdown();
    ASSERT_TRUE(runtime->WaitStopped(kWaitMs));
    EXPECT_EQ(runtime->Status().finalizingCount, 1u);
}
}  // namespace
