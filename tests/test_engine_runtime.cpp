// Behavior of the runtime engine through its interface (llm-wiki/library/04 "runtime/core", 07): a real
// engine thread over fake helper children, fixed settings and a fake screenshot capture. Nothing spawns,
// registers hotkeys or opens windows. Waits are on runtime events with generous bounds, never sleeps.
#include <gtest/gtest.h>

#include "runtime/core/runtime_internal.h"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace ce::engine;
using ce::runtime::HostChild;

constexpr uint32_t kWaitMs = 20000;  // upper bound only: every wait ends on an event

// What the fake children did and how they answer. Shared with the test; the engine thread owns the fake.
struct ChildrenScript {
    std::mutex mutex;
    std::vector<std::string> calls;
    bool injectStarts = true;
    bool mediaStarts = true;
    bool shutdownSucceeds = true;
    HANDLE injectGate = nullptr;  // when set, Ensure(Inject) waits for it (holds the engine in STARTING)
    std::atomic<bool> live{false};
    std::map<HostChild, bool> present;

    void Record(std::string call) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(std::move(call));
    }
    std::vector<std::string> Calls() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    bool Called(const std::string& call) {
        for (const std::string& entry : Calls())
            if (entry == call)
                return true;
        return false;
    }
};

class FakeChildren final : public ce::runtime::HostChildren {
public:
    explicit FakeChildren(std::shared_ptr<ChildrenScript> script) : script_(std::move(script)) {}
    bool Ensure(HostChild child, uint32_t) override {
        script_->Record("ensure-" + Name(child));
        if (child == HostChild::Inject && script_->injectGate)
            WaitForSingleObject(script_->injectGate, kWaitMs);
        const bool ok = child == HostChild::Inject ? script_->injectStarts
                        : child == HostChild::Media ? script_->mediaStarts
                                                    : true;
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->present[child] = ok;
        return ok;
    }
    bool Ready(HostChild child) const override { return Present(child); }
    bool Present(HostChild child) const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        const auto it = script_->present.find(child);
        return it != script_->present.end() && it->second;
    }
    bool Running(HostChild child) const override { return Present(child); }
    bool Send(HostChild child, ProcessCommand command, const char*, ProcessResponse* response, uint32_t) override {
        script_->Record("send-" + Name(child) + "-" + std::to_string(static_cast<int>(command)));
        if (!Present(child))
            return false;
        if (response)
            *response = ProcessResponse::Ack;
        return true;
    }
    void SendToAll(ProcessCommand command) override { script_->Record("send-all-" + std::to_string(static_cast<int>(command))); }
    void RetireMedia(uint64_t) override {
        script_->Record("retire-media");
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->present[HostChild::Media] = false;
    }
    std::vector<ce::runtime::FinalizedMedia> CollectFinalizedMedia() override { return {}; }
    uint32_t FinalizingCount() const override { return 0; }
    ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t) override {
        script_->Record("stop-" + Name(child));
        return ce::ipc::CommandOutcome::Accepted;
    }
    void Service(ce::runtime::AuxiliaryServices, const std::function<void()>& beforeRecovery) override {
        if (beforeRecovery)
            beforeRecovery();
    }
    void ReconfigureServices(ce::runtime::AuxiliaryServices) override { script_->Record("reconfigure"); }
    bool StopSensorsForSetup() override {
        script_->Record("stop-sensors");
        return true;
    }
    bool Shutdown() override {
        script_->Record("shutdown");
        return script_->shutdownSucceeds;
    }
    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent) override { return {}; }
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType, uint64_t) override { return {}; }
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override {
        observation = {};
        observation.live = script_->live.load();  // no live stamp: never stale against the start
        return {};
    }
    void ClearMediaFailure(uint32_t, bool) override {}
    ce::ipc::InjectControlChannel InjectChannel() const override { return ce::ipc::InjectControlChannel(); }

private:
    static std::string Name(HostChild child) {
        switch (child) {
            case HostChild::Inject:
                return "inject";
            case HostChild::Media:
                return "media";
            case HostChild::Logger:
                return "logger";
            case HostChild::Sensors:
                return "sensors";
        }
        return "?";
    }
    std::shared_ptr<ChildrenScript> script_;
};

// Fixed settings; a test can publish one replacement, which the engine fans out.
struct SettingsScript {
    std::mutex mutex;
    AppConfig config;
    std::optional<AppConfig> previousToPublish;
};

class FakeSettings final : public SettingsSource {
public:
    explicit FakeSettings(std::shared_ptr<SettingsScript> script) : script_(std::move(script)) {
        script_->config.logLevel = LogLevel::Off;  // no logger helper, no sensors
    }
    const AppConfig& Current() const override { return script_->config; }
    std::optional<AppConfig> Poll() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        std::optional<AppConfig> previous = std::move(script_->previousToPublish);
        script_->previousToPublish.reset();
        return previous;
    }
    uint32_t WaitMs() const override { return 5; }  // service often; every assertion still waits on an event

private:
    std::shared_ptr<SettingsScript> script_;
};

struct Harness {
    std::shared_ptr<ChildrenScript> children = std::make_shared<ChildrenScript>();
    std::shared_ptr<SettingsScript> settings = std::make_shared<SettingsScript>();
    std::atomic<int> captures{0};
    HANDLE captureGate = nullptr;  // when set, the screenshot capture waits for it

    std::unique_ptr<Runtime> Start() {
        RuntimeParts parts;
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

Command Make(CommandKind kind, int32_t mode = CE_MODE_VIDEO) {
    Command command;
    command.kind = kind;
    command.mode = mode;
    return command;
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

TEST(EngineRuntimeTest, AFailedHelperShutdownStillStopsTheEngine) {
    Harness harness;
    harness.children->shutdownSucceeds = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    runtime->RequestShutdown();
    EXPECT_TRUE(runtime->WaitStopped(kWaitMs));
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
}  // namespace
