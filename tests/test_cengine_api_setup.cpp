// The setup entry points of the C ABI (llm-wiki/library/03 "Platform setup"), called exactly as a client calls them.
// The engine is assembled from fakes through the test seam (runtime/api/capi_testing.h): no helper spawns, the setup
// results come from a fake backend. Waits are on runtime events with generous bounds; nothing sleeps.
#include "include/cengine/cengine.h"

#include "runtime/api/capi_testing.h"
#include "runtime/core/runtime_internal.h"
#include "runtime/platform/setup_actions.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace ce::engine;
using ce::runtime::HostChild;
namespace testing_seam = ce::capi::testing;

constexpr uint32_t kWaitMs = 20000;   // upper bound only: every wait ends on an event
constexpr int kMaxEventReads = 4096;  // bound on one WaitFor: a runaway event stream fails the test, not hangs it

// Helpers that start without spawning anything. Ensure() holds at `gate` when one is set (keeps the runtime in
// STARTING); every other answer is the trivial one.
class NoHelpers final : public ce::runtime::HostChildren {
public:
    explicit NoHelpers(HANDLE gate) : gate_(gate) {}
    bool Ensure(HostChild child, uint32_t) override {
        if (gate_)
            WaitForSingleObject(gate_, kWaitMs);
        present_[child] = true;
        return true;
    }
    bool Ready(HostChild child) const override { return Present(child); }
    bool Present(HostChild child) const override {
        const auto it = present_.find(child);
        return it != present_.end() && it->second;
    }
    bool Running(HostChild child) const override { return Present(child); }
    bool Send(HostChild, ProcessCommand, const char*, ProcessResponse*, uint32_t) override { return false; }
    void SendToAll(ProcessCommand) override {}
    void RetireMedia(uint64_t) override {}
    std::vector<ce::runtime::FinalizedMedia> CollectFinalizedMedia() override { return {}; }
    uint32_t FinalizingCount() const override { return 0; }
    ce::ipc::CommandOutcome StopRecording(HostChild, uint32_t) override { return ce::ipc::CommandOutcome::Accepted; }
    void Service(ce::runtime::AuxiliaryServices, const std::function<void()>& beforeRecovery) override {
        if (beforeRecovery)
            beforeRecovery();
    }
    void ReconfigureServices(ce::runtime::AuxiliaryServices) override {}
    bool StopSensorsForSetup() override { return true; }
    void ResumeSensorsAfterSetup() override {}
    bool Shutdown() override { return true; }
    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent) override { return {}; }
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType, uint64_t) override { return {}; }
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override {
        observation = {};
        return {};
    }
    void ClearMediaFailure(uint32_t, bool) override {}
    ce::ipc::InjectControlChannel InjectChannel() const override { return ce::ipc::InjectControlChannel(); }

private:
    HANDLE gate_;
    std::map<HostChild, bool> present_;
};

class FixedSettings final : public SettingsSource {
public:
    FixedSettings() { config_.logLevel = LogLevel::Off; }  // no logger helper, no sensors
    const AppConfig& Current() const override { return config_; }
    std::optional<AppConfig> Poll() override { return std::nullopt; }
    uint32_t WaitMs() const override { return 5; }

private:
    AppConfig config_;
};

// The fake OS seam: records the requests, answers the driver request with `driverResult` and hands out
// `driverDone` (the finished result) once, when the engine polls for it.
class FakeApiSetup final : public ce::runtime::SetupBackend {
public:
    ce::startup::AutostartTarget ClientTarget() override {
        return ce::runtime::ClientAutostartTarget(L"C:\\Test\\client.exe", L"");
    }
    unsigned long RequestElevationService(bool) override { return ERROR_SUCCESS; }
    unsigned long RequestClientAutostart(const ce::startup::AutostartTarget&, bool) override { return ERROR_SUCCESS; }
    std::optional<unsigned long> TakeSetupResult() override { return std::nullopt; }
    std::optional<unsigned long> WaitSetupResult() override { return std::nullopt; }
    ce::startup::ElevationServiceState QueryElevationServiceState() override {
        return ce::startup::ElevationServiceState::Running;
    }
    ce::startup::AutostartState QueryClientAutostart(const ce::startup::AutostartTarget&) override {
        return ce::startup::AutostartState::UserRun;
    }
    unsigned long RequestDriverInstall() override { return Request("RequestDriverInstall"); }
    unsigned long RequestDriverUninstall() override { return Request("RequestDriverUninstall"); }
    std::optional<unsigned long> TakeDriverSetupResult() override {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(driverDone, std::nullopt);
    }
    std::optional<unsigned long> WaitDriverSetupResult() override { return std::nullopt; }
    bool IsDriverInstalled() override { return true; }
    bool LaunchElevated() override { return false; }
    bool ElevationServicePreferred() override { return false; }
    bool SensorLibraryPresent() override { return true; }
    bool SensorOfferSuppressed() override { return false; }
    unsigned long RequestLaunchElevated(bool enable) override {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(enable ? "RequestLaunchElevated(on)" : "RequestLaunchElevated(off)");
        return ERROR_SUCCESS;
    }
    void DismissSensorOffer() override {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back("DismissSensorOffer");
    }

    std::mutex mutex;
    std::vector<std::string> calls;
    unsigned long driverResult = ERROR_SUCCESS;
    std::optional<unsigned long> driverDone;

private:
    unsigned long Request(const char* call) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(call);
        return driverResult;
    }
};

// One event, copied out so the test may read it after the release.
struct SeenCompletion {
    uint64_t request = 0;
    ce_status_t status = CE_OK;
};

// Reads and releases events until a COMMAND_COMPLETED for `request` arrives (or the bound passes).
std::optional<SeenCompletion> CompletionOf(ce_runtime_t* rt, uint64_t request) {
    for (int reads = 0; reads < kMaxEventReads; ++reads) {
        const ce_event_t* event = nullptr;
        if (ce_runtime_next_event(rt, kWaitMs, &event) != CE_OK || !event)
            return std::nullopt;
        SeenCompletion copy;
        copy.request = event->request;
        copy.status = event->status;
        const bool match = event->type == CE_EVENT_COMMAND_COMPLETED && event->request == request;
        EXPECT_EQ(ce_runtime_release_event(rt, event), CE_OK);
        if (match)
            return copy;
    }
    return std::nullopt;
}

bool IsRuntimeState(const ce_event_t& event, int32_t state) {
    return event.type == CE_EVENT_RUNTIME_STATE && event.data.runtime.state == state;
}

// Waits for the runtime to reach `state`, releasing whatever came before it.
bool WaitForState(ce_runtime_t* rt, int32_t state) {
    for (int reads = 0; reads < kMaxEventReads; ++reads) {
        const ce_event_t* event = nullptr;
        if (ce_runtime_next_event(rt, kWaitMs, &event) != CE_OK || !event)
            return false;
        const bool match = IsRuntimeState(*event, state);
        EXPECT_EQ(ce_runtime_release_event(rt, event), CE_OK);
        if (match)
            return true;
    }
    return false;
}

class CengineApiSetupTest : public ::testing::Test {
protected:
    void SetUp() override {
        gate_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        testing_seam::SetRuntimeFactory([this](const RuntimeOptions& options, ce_status_t* status) {
            RuntimeParts parts;
            parts.options = options;
            HANDLE gate = gate_;
            parts.makeChildren = [gate](void (*)(), bool (*)()) {
                return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<NoHelpers>(gate));
            };
            parts.settings = std::make_unique<FixedSettings>();
            parts.setup = &setup;
            return RuntimeFactory::Assemble(std::move(parts), status);
        });
    }

    void TearDown() override {
        if (gate_)
            SetEvent(gate_);  // open the gate first: a held start would hold this test for the whole bound
        if (rt_) {
            ce_runtime_request_shutdown(rt_);
            ce_runtime_shutdown(rt_, kWaitMs);
            ce_runtime_destroy(rt_);
            rt_ = nullptr;
        }
        testing_seam::SetRuntimeFactory({});
        if (gate_)
            CloseHandle(gate_);
        gate_ = nullptr;
    }

    // A runtime that is still STARTING until the gate opens.
    ce_runtime_t* CreateHeld() {
        ce_runtime_desc_t desc{};
        EXPECT_EQ(ce_runtime_desc_init(&desc, sizeof(desc), CE_API_VERSION), CE_OK);
        desc.client_name = "test-client";
        ce_runtime_t* rt = nullptr;
        EXPECT_EQ(ce_runtime_create(&desc, &rt), CE_OK);
        rt_ = rt;
        return rt;
    }

    void OpenGateAndWaitReady(ce_runtime_t* rt) {
        SetEvent(gate_);
        ASSERT_TRUE(WaitForState(rt, CE_RUNTIME_READY));
    }

    HANDLE gate_ = nullptr;
    FakeApiSetup setup;

private:
    ce_runtime_t* rt_ = nullptr;
};

TEST_F(CengineApiSetupTest, SetupRequestNeedsARuntimeAndAKnownActionAndAReadyRuntime) {
    EXPECT_EQ(ce_setup_request(nullptr, CE_SETUP_SENSOR_DRIVER_INSTALL, nullptr), CE_E_INVALID_ARGUMENT);

    ce_runtime_t* rt = CreateHeld();
    ASSERT_NE(rt, nullptr);
    ce_request_t request = 99;
    EXPECT_EQ(ce_setup_request(rt, CE_SETUP_SENSOR_DRIVER_INSTALL, &request), CE_E_INVALID_STATE)
        << "not READY yet";
    EXPECT_EQ(request, 0u) << "a refused request has no id";
    EXPECT_EQ(ce_setup_request(rt, 0, &request), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_setup_request(rt, 10, nullptr), CE_E_INVALID_ARGUMENT);
    OpenGateAndWaitReady(rt);
    EXPECT_EQ(ce_setup_request(rt, -1, nullptr), CE_E_INVALID_ARGUMENT);
    ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
}

TEST_F(CengineApiSetupTest, AnAdmittedRequestCompletesOnceWithTheBackendsResult) {
    ce_runtime_t* rt = CreateHeld();
    ASSERT_NE(rt, nullptr);
    OpenGateAndWaitReady(rt);
    {
        std::lock_guard<std::mutex> lock(setup.mutex);
        setup.driverDone = ERROR_SUCCESS;
    }
    ce_request_t request = 0;
    ASSERT_EQ(ce_setup_request(rt, CE_SETUP_SENSOR_DRIVER_INSTALL, &request), CE_OK);
    ASSERT_NE(request, 0u);
    const std::optional<SeenCompletion> done = CompletionOf(rt, request);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, CE_OK);
    {
        std::lock_guard<std::mutex> lock(setup.mutex);
        EXPECT_EQ(setup.calls, std::vector<std::string>{"RequestDriverInstall"});
    }
}

TEST_F(CengineApiSetupTest, BusyAndDeclinedRequestsCompleteWithTheirOwnStatuses) {
    ce_runtime_t* rt = CreateHeld();
    ASSERT_NE(rt, nullptr);
    OpenGateAndWaitReady(rt);

    {
        std::lock_guard<std::mutex> lock(setup.mutex);
        setup.driverResult = ERROR_BUSY;
    }
    ce_request_t busy = 0;
    ASSERT_EQ(ce_setup_request(rt, CE_SETUP_SENSOR_DRIVER_INSTALL, &busy), CE_OK);
    ASSERT_TRUE(CompletionOf(rt, busy).has_value());

    {
        std::lock_guard<std::mutex> lock(setup.mutex);
        setup.driverResult = ERROR_SUCCESS;
        setup.driverDone = ERROR_CANCELLED;
    }
    ce_request_t declined = 0;
    ASSERT_EQ(ce_setup_request(rt, CE_SETUP_SENSOR_DRIVER_INSTALL, &declined), CE_OK);
    const std::optional<SeenCompletion> done = CompletionOf(rt, declined);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, CE_E_CANCELED);
}

TEST_F(CengineApiSetupTest, GetStatusReadsTheBackendBeforeAndAfterReady) {
    ce_runtime_t* rt = CreateHeld();
    ASSERT_NE(rt, nullptr);

    ce_setup_status_t status{};
    status.struct_size = sizeof(status);
    ASSERT_EQ(ce_setup_get_status(rt, &status), CE_OK) << "callable while STARTING, from any thread";
    EXPECT_EQ(status.sensor_driver_installed, 1u);
    EXPECT_EQ(status.elevation_service_state, 2);
    EXPECT_EQ(status.client_autostart, 1u);
    EXPECT_EQ(status.sensor_driver_offer, 0u) << "the driver is installed: nothing to offer";
    EXPECT_EQ(status.launch_elevated, 0u);
    EXPECT_EQ(status.elevation_service_preferred, 0u);
    OpenGateAndWaitReady(rt);
    ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
}

TEST_F(CengineApiSetupTest, GetStatusRefusesBadArgumentsAndCopiesOnlyWhatFitsTheExtent) {
    ce_runtime_t* rt = CreateHeld();
    ASSERT_NE(rt, nullptr);
    ce_setup_status_t status{};
    status.struct_size = sizeof(status);
    EXPECT_EQ(ce_setup_get_status(nullptr, &status), CE_E_INVALID_ARGUMENT);
    EXPECT_EQ(ce_setup_get_status(rt, nullptr), CE_E_INVALID_ARGUMENT);
    ce_setup_status_t tooSmall{};
    tooSmall.struct_size = static_cast<uint32_t>(offsetof(ce_setup_status_t, sensor_driver_installed));
    EXPECT_EQ(ce_setup_get_status(rt, &tooSmall), CE_E_INVALID_ARGUMENT);

    // An older client knows the driver field only: the rest of its buffer (and the guard) stays as it was.
    struct Guarded {
        ce_setup_status_t status;
        unsigned char tail[16];
    } guarded;
    std::memset(&guarded, 0xAB, sizeof(guarded));
    guarded.status.struct_size = static_cast<uint32_t>(offsetof(ce_setup_status_t, elevation_service_state));
    ASSERT_EQ(ce_setup_get_status(rt, &guarded.status), CE_OK);
    EXPECT_EQ(guarded.status.sensor_driver_installed, 1u);
    EXPECT_EQ(guarded.status.struct_size, static_cast<uint32_t>(offsetof(ce_setup_status_t, elevation_service_state)));
    const auto* raw = reinterpret_cast<const unsigned char*>(&guarded);
    for (size_t i = offsetof(ce_setup_status_t, elevation_service_state); i < sizeof(guarded); ++i)
        ASSERT_EQ(raw[i], 0xAB) << "byte " << i << " is beyond the caller's extent";
    ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
}

TEST_F(CengineApiSetupTest, TheOfferAnswerAndTheLaunchElevatedPreferenceAreAdmittedAsSetupRequests) {
    ce_runtime_t* rt = CreateHeld();
    ASSERT_NE(rt, nullptr);
    OpenGateAndWaitReady(rt);
    EXPECT_EQ(static_cast<int>(sizeof(ce_setup_status_t)), 28) << "the appended status fields";

    ce_request_t dismissed = 0;
    ASSERT_EQ(ce_setup_request(rt, CE_SETUP_SENSOR_OFFER_DISMISS, &dismissed), CE_OK);
    const std::optional<SeenCompletion> done = CompletionOf(rt, dismissed);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ(done->status, CE_OK);
    {
        std::lock_guard<std::mutex> lock(setup.mutex);
        ASSERT_FALSE(setup.calls.empty());
        EXPECT_EQ(setup.calls.front(), "DismissSensorOffer");
    }

    ce_request_t elevated = 0;
    EXPECT_EQ(ce_setup_request(rt, CE_SETUP_LAUNCH_ELEVATED_ON, &elevated), CE_OK);
    EXPECT_NE(elevated, 0u);
    EXPECT_EQ(ce_setup_request(rt, 10, nullptr), CE_E_INVALID_ARGUMENT);
    ASSERT_EQ(ce_runtime_request_shutdown(rt), CE_OK);
}

}  // namespace
