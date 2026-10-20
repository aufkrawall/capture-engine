// Behavior of the runtime engine through its interface (llm-wiki/library/04 "runtime/core", 07): a real
// engine thread over fake helper children, fixed settings and a fake screenshot capture. Nothing spawns,
// registers hotkeys or opens windows. Waits are on runtime events with generous bounds, never sleeps.
#include <gtest/gtest.h>

#include "common/ipc/media_finalization.h"
#include "engine_test_fakes.h"
#include "runtime/core/runtime_internal.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace ce::engine;
using ce::runtime::HostChild;
using engine_test::ChildrenScript;
using engine_test::FakeChildren;

constexpr uint32_t kWaitMs = 20000;  // upper bound only: every wait ends on an event

// Fixed settings; a test can publish one replacement, which the engine fans out.
struct SettingsScript {
    std::mutex mutex;
    LogLevel logLevel = LogLevel::Off;  // Off: no logger helper; a test sets Info to start one
    AppConfig config;
    uint64_t revision = 0;              // bumps with every published replacement, as the store's does
    std::optional<AppConfig> previousToPublish;
};

class FakeSettings final : public SettingsSource {
public:
    explicit FakeSettings(std::shared_ptr<SettingsScript> script) : script_(std::move(script)) {
        script_->config.logLevel = script_->logLevel;
    }
    const AppConfig& Current() const override { return script_->config; }
    std::optional<AppConfig> Poll() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        std::optional<AppConfig> previous = std::move(script_->previousToPublish);
        script_->previousToPublish.reset();
        if (previous)
            ++script_->revision;
        return previous;
    }
    uint32_t WaitMs() const override { return 5; }  // service often; every assertion still waits on an event
    uint64_t Revision() const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        return script_->revision;
    }

private:
    std::shared_ptr<SettingsScript> script_;
};

struct Harness {
    std::shared_ptr<ChildrenScript> children = std::make_shared<ChildrenScript>();
    std::shared_ptr<SettingsScript> settings = std::make_shared<SettingsScript>();
    std::atomic<int> captures{0};
    HANDLE captureGate = nullptr;  // when set, the screenshot capture waits for it

    // The runtime watches its settings file, as the application's does, unless a test says otherwise.
    std::unique_ptr<Runtime> Start(uint32_t features = CE_FEATURE_WATCH_SETTINGS) {
        RuntimeParts parts;
        parts.options.features = features;
        auto script = children;
        parts.makeChildren = [script](void (*)(), bool (*)()) {
            return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<FakeChildren>(script));
        };
        parts.settings = std::make_unique<FakeSettings>(settings);
        parts.capture = [this](const std::string&, const std::string&) {
            ++captures;
            if (captureGate)
                WaitForSingleObject(captureGate, kWaitMs);
            return ce::runtime::ScreenshotCapture{true, {"C:\\shots\\a.png", "C:\\shots\\a.avif"}};
        };
        ce_status_t status = CE_E_INTERNAL;
        std::unique_ptr<Runtime> runtime = RuntimeFactory::Assemble(std::move(parts), &status);
        EXPECT_EQ(status, CE_OK);
        return runtime;
    }
};

struct SeenEvent {
    int32_t type = 0;
    uint64_t request = 0;
    ce_status_t status = CE_OK;
    int32_t state = 0;
    uint32_t failure = 0;
    uint32_t pathCount = 0;
    uint64_t recordingId = 0;
    uint32_t helper = 0;
};

// Reads events until `match` accepts one (returned) or the bound passes; every event is released.
std::optional<SeenEvent> WaitFor(Runtime& runtime, const std::function<bool(const ce_event_t&)>& match,
                                 std::vector<SeenEvent>* seen = nullptr) {
    for (;;) {
        const ce_event_t* event = nullptr;
        if (runtime.Events().Next(kWaitMs, &event) != CE_OK)
            return std::nullopt;
        SeenEvent copy;
        copy.type = event->type;
        copy.request = event->request;
        copy.status = event->status;
        if (event->type == CE_EVENT_RUNTIME_STATE)
            copy.state = event->data.runtime.state;
        if (event->type == CE_EVENT_RECORDING_STATE) {
            copy.state = event->data.recording.state;
            copy.failure = event->data.recording.failure;
            copy.recordingId = event->data.recording.recording_id;
        }
        if (event->type == CE_EVENT_HELPER) {
            copy.helper = event->data.helper.helper;
            copy.state = event->data.helper.state;
        }
        if (event->type == CE_EVENT_SCREENSHOT)
            copy.pathCount = event->data.screenshot.path_count;
        const bool matched = match(*event);
        runtime.Events().Release(event);
        if (seen)
            seen->push_back(copy);
        if (matched)
            return copy;
    }
}

bool IsRuntimeState(const ce_event_t& event, int32_t state) {
    return event.type == CE_EVENT_RUNTIME_STATE && event.data.runtime.state == state;
}

bool IsHelper(const ce_event_t& event, uint32_t helper, int32_t state) {
    return event.type == CE_EVENT_HELPER && event.data.helper.helper == helper && event.data.helper.state == state;
}

// The HELPER states announced for one helper, in order.
std::vector<int32_t> HelperStates(const std::vector<SeenEvent>& seen, uint32_t helper) {
    std::vector<int32_t> states;
    for (const SeenEvent& event : seen)
        if (event.type == CE_EVENT_HELPER && event.helper == helper)
            states.push_back(event.state);
    return states;
}

// The fake helper's readiness toggle: a present child that is down is a crash the engine must see.
void SetHelperDown(Harness& harness, HostChild child, bool down) {
    std::lock_guard<std::mutex> lock(harness.children->mutex);
    harness.children->down[child] = down;
}

Command Make(CommandKind kind, int32_t mode = CE_MODE_VIDEO) {
    Command command;
    command.kind = kind;
    command.mode = mode;
    return command;
}

// A retired media child that finalized with `status`: its exit code and its slot result, resolved as the spawner does.
ce::runtime::FinalizedMedia Finalized(uint64_t request, ce::media_result::Status status,
                                      const std::string& path = {}) {
    ce::media_result::Result result;
    result.status = status;
    result.finalizationCount = 1;
    if (!path.empty())
        result.paths.push_back(path);
    ce::runtime::FinalizedMedia media;
    media.recordingRequest = request;
    media.resolution = ce::media_result::Resolve(ce::media_result::EncodeExitCode(status, 0), &result);
    return media;
}

// A retired media child that was killed before it published anything (exit code 1, no slot result).
ce::runtime::FinalizedMedia Killed(uint64_t request) {
    ce::runtime::FinalizedMedia media;
    media.recordingRequest = request;
    media.resolution = ce::media_result::Resolve(1u, nullptr);
    return media;
}

struct FinalizedSeen {
    ce_status_t status = CE_E_INTERNAL;
    bool hasPath = false;
    std::string path;
    std::string message;
};

// Reads `count` FINALIZED events (bounded by the wait), keyed by the recording id each one names.
void CollectFinalized(Runtime& runtime, size_t count, std::map<uint64_t, FinalizedSeen>& out) {
    for (size_t i = 0; i < count; ++i) {
        const auto event = WaitFor(runtime, [&](const ce_event_t& e) {
            if (e.type != CE_EVENT_RECORDING_FINALIZED)
                return false;
            FinalizedSeen& entry = out[e.data.finalized.recording_id];
            entry.status = e.status;
            entry.hasPath = e.data.finalized.output_path != nullptr;
            entry.path = entry.hasPath ? e.data.finalized.output_path : "";
            entry.message = e.message ? e.message : "";
            return true;
        });
        ASSERT_TRUE(event.has_value()) << "finalized event " << i << " of " << count;
    }
}

// Starts a recording over a live helper and waits until it is live; returns its recording id (0 on failure).
uint64_t StartAndWaitLive(Runtime& runtime) {
    EXPECT_EQ(runtime.Submit(Make(CommandKind::StartRecording)).status, CE_OK);
    const auto live = WaitFor(runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_LIVE;
    });
    return live ? live->recordingId : 0;
}

void StopAndWaitIdle(Runtime& runtime) {
    ASSERT_EQ(runtime.Submit(Make(CommandKind::StopRecording)).status, CE_OK);
    ASSERT_TRUE(WaitFor(runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_IDLE;
    }));
}

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
                             CommandKind::StartRecording, CommandKind::StopRecording, CommandKind::StopSensorsForSetup}) {
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
    EXPECT_TRUE(harness.children->Called("stop-sensors"));
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
