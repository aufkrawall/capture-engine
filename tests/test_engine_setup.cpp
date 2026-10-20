// Platform setup through the engine (llm-wiki/library/04 runtime/platform): admission, the sensor helper held
// around driver removal, exactly-once completions and the driver offer, over a fake SetupBackend.
#include <gtest/gtest.h>

#include "engine_runtime_harness.h"

using namespace engine_runtime_test;

namespace {
Command SetupCommand(int32_t action) {
    Command command;
    command.kind = CommandKind::Setup;
    command.setupAction = action;
    return command;
}

constexpr int32_t kInstall = static_cast<int32_t>(ce::runtime::SetupAction::SensorDriverInstall);
constexpr int32_t kUninstall = static_cast<int32_t>(ce::runtime::SetupAction::SensorDriverUninstall);

// The children-and-driver log entries recorded after `from` (the position before the request was admitted).
std::vector<std::string> CallsAfter(Harness& harness, size_t from) {
    const std::vector<std::string> calls = harness.children->Calls();
    return std::vector<std::string>(calls.begin() + static_cast<std::ptrdiff_t>(from), calls.end());
}

// Waits for the completion of `request` and returns its status; the bound fails the test instead of hanging it.
std::optional<ce_status_t> CompletionOf(Runtime& runtime, uint64_t request, std::vector<SeenEvent>* seen = nullptr) {
    auto event = WaitFor(runtime, [request](const ce_event_t& e) {
        return e.type == CE_EVENT_COMMAND_COMPLETED && e.request == request;
    }, seen);
    if (!event)
        return std::nullopt;
    return event->status;
}
}  // namespace

TEST(EngineRuntimeTest, SetupRequestsAreAdmittedOnlyInReadyWithAKnownAction) {
    Harness harness;
    harness.children->injectGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    // An unknown action is refused at admission, before the state is looked at.
    EXPECT_EQ(runtime->Submit(SetupCommand(0)).status, CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(runtime->Submit(SetupCommand(10)).status, CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(runtime->Submit(SetupCommand(kInstall)).status, CE_E_INVALID_STATE);
    SetEvent(harness.children->injectGate);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    EXPECT_EQ(runtime->Submit(SetupCommand(-1)).status, CE_E_INVALID_ARGUMENT);
    runtime->RequestShutdown();
    runtime.reset();
    CloseHandle(harness.children->injectGate);
}

TEST(EngineRuntimeTest, UninstallStopsTheSensorHelperBeforeTheDriverIsRemovedAndResumesIt) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    {
        std::lock_guard<std::mutex> lock(harness.setup->mutex);
        harness.setup->driverDone = ERROR_SUCCESS;
    }
    const size_t before = harness.children->Calls().size();
    const Admission admission = runtime->Submit(SetupCommand(kUninstall));
    ASSERT_EQ(admission.status, CE_OK);
    const std::optional<ce_status_t> status = CompletionOf(*runtime, admission.request);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, CE_OK);
    // Order: the helper is stopped first, the driver is removed next, the helper is released when the action is over.
    EXPECT_EQ(CallsAfter(harness, before),
              (std::vector<std::string>{"stop-sensors", "request-driver-uninstall", "resume-sensors"}));
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, AFailedSensorStopStartsNoDriverSetupAndReleasesTheHelper) {
    Harness harness;
    harness.children->sensorStopSucceeds = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const size_t before = harness.children->Calls().size();
    const Admission admission = runtime->Submit(SetupCommand(kUninstall));
    ASSERT_EQ(admission.status, CE_OK);
    const std::optional<ce_status_t> status = CompletionOf(*runtime, admission.request);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, CE_E_PROCESS);
    EXPECT_EQ(CallsAfter(harness, before), (std::vector<std::string>{"stop-sensors", "resume-sensors"}));
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, AnInstallRefusedAsBusyCompletesBusyAndTouchesNoSensorHelper) {
    Harness harness;
    {
        std::lock_guard<std::mutex> lock(harness.setup->mutex);
        harness.setup->driverResult = ERROR_BUSY;
    }
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const size_t before = harness.children->Calls().size();
    const Admission admission = runtime->Submit(SetupCommand(kInstall));
    ASSERT_EQ(admission.status, CE_OK);
    const std::optional<ce_status_t> status = CompletionOf(*runtime, admission.request);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, CE_E_BUSY);
    EXPECT_EQ(CallsAfter(harness, before), (std::vector<std::string>{"request-driver-install"}));
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, ADeclinedPromptCompletesCanceledAndAFailureCompletesWithAnError) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    {
        std::lock_guard<std::mutex> lock(harness.setup->mutex);
        harness.setup->driverDone = ERROR_CANCELLED;
    }
    const Admission declined = runtime->Submit(SetupCommand(kInstall));
    ASSERT_EQ(declined.status, CE_OK);
    EXPECT_EQ(CompletionOf(*runtime, declined.request), CE_E_CANCELED);
    {
        std::lock_guard<std::mutex> lock(harness.setup->mutex);
        harness.setup->driverDone = ERROR_ACCESS_DENIED;
    }
    const Admission failed = runtime->Submit(SetupCommand(kInstall));
    ASSERT_EQ(failed.status, CE_OK);
    EXPECT_EQ(CompletionOf(*runtime, failed.request), CE_E_PROCESS);
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, AnActionInFlightAtShutdownCompletesOnceWithItsRealResult) {
    Harness harness;
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lock(harness.setup->mutex);
        harness.setup->waitGate = gate;
        harness.setup->waitResult = ERROR_SUCCESS;
        harness.setup->started = started;
    }
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const Admission admission = runtime->Submit(SetupCommand(kInstall));
    ASSERT_EQ(admission.status, CE_OK);
    // The action is in flight once the backend accepted it; no result is ever collected before shutdown.
    ASSERT_EQ(WaitForSingleObject(started, kWaitMs), WAIT_OBJECT_0);
    runtime->RequestShutdown();
    SetEvent(gate);
    std::vector<SeenEvent> seen;
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }, &seen));
    int completions = 0;
    for (const SeenEvent& event : seen) {
        if (event.type == CE_EVENT_COMMAND_COMPLETED && event.request == admission.request) {
            ++completions;
            EXPECT_EQ(event.status, CE_OK);
        }
    }
    EXPECT_EQ(completions, 1);
    runtime.reset();
    CloseHandle(gate);
    CloseHandle(started);
}

TEST(EngineRuntimeTest, ASetupRequestWithoutABackendCompletesUnsupported) {
    Harness harness;
    harness.setupEnabled = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    ce::runtime::SetupStatus status;
    EXPECT_FALSE(runtime->SetupState(status));
    const Admission admission = runtime->Submit(SetupCommand(kInstall));
    ASSERT_EQ(admission.status, CE_OK);
    EXPECT_EQ(CompletionOf(*runtime, admission.request), CE_E_UNSUPPORTED);
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, TheSetupStatusIsReadFromTheBackendWithoutTheEngine) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ce::runtime::SetupStatus status;
    ASSERT_TRUE(runtime->SetupState(status));
    EXPECT_TRUE(status.sensorDriverInstalled);
    EXPECT_EQ(status.elevationServiceState, static_cast<int32_t>(ce::startup::ElevationServiceState::Running));
    EXPECT_TRUE(status.clientAutostart);
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, DismissingTheDriverOfferIsRecordedAtOnceAndNeverReachesTheWorker) {
    Harness harness;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    const size_t before = harness.children->Calls().size();
    const Admission admission =
        runtime->Submit(SetupCommand(static_cast<int32_t>(ce::runtime::SetupAction::SensorOfferDismiss)));
    ASSERT_EQ(admission.status, CE_OK);
    EXPECT_EQ(CompletionOf(*runtime, admission.request), CE_OK);
    {
        std::lock_guard<std::mutex> lock(harness.setup->mutex);
        EXPECT_EQ(harness.setup->dismissed, 1);
    }
    EXPECT_EQ(CallsAfter(harness, before), (std::vector<std::string>{"dismiss-sensor-offer"}));
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, TheDriverOfferFollowsTheAppliedSettingsAndTheDriverState) {
    Harness harness;
    harness.setup->driverInstalled = false;
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    ce::runtime::SetupStatus status;
    ASSERT_TRUE(runtime->SetupState(status));
    EXPECT_TRUE(status.sensorDriverOffer) << "CPU sensors on automatic, library present, driver absent, not declined";
    EXPECT_FALSE(status.launchElevated);
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}

TEST(EngineRuntimeTest, SensingOffLeavesNoDriverOfferWhateverTheDriverState) {
    Harness harness;
    harness.setup->driverInstalled = false;
    harness.settings->config.hardwareSensors.enabled = "off";
    auto runtime = harness.Start();
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_READY); }));
    ce::runtime::SetupStatus status;
    ASSERT_TRUE(runtime->SetupState(status));
    EXPECT_FALSE(status.sensorDriverOffer);
    runtime->RequestShutdown();
    ASSERT_TRUE(WaitFor(*runtime, [](const ce_event_t& e) { return IsRuntimeState(e, CE_RUNTIME_STOPPED); }));
}
