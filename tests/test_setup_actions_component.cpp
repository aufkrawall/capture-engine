#include "runtime/platform/setup_actions.h"

#include "common/config/config.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using ce::runtime::SetupAction;
using ce::runtime::SetupActions;
using ce::runtime::SetupBackend;
using ce::runtime::SetupCompletion;
using ce::runtime::SetupStatus;
using ce::startup::AutostartState;
using ce::startup::AutostartTarget;
using ce::startup::ElevationServiceState;

namespace {

constexpr wchar_t kClientExecutable[] = LR"(C:\Program Files\Client\client.exe)";
constexpr wchar_t kClientArguments[] = L"--minimized";
constexpr unsigned long kSuccess = ERROR_SUCCESS;
constexpr unsigned long kBusy = ERROR_BUSY;
constexpr unsigned long kInvalidParameter = ERROR_INVALID_PARAMETER;
constexpr unsigned long kFileNotFound = ERROR_FILE_NOT_FOUND;
constexpr unsigned long kCancelled = ERROR_CANCELLED;

// Fake OS seam. Every call is recorded by name. A worker result is queued by the test (pendingSetup or pendingDriver)
// and handed out once by Take* (polling) or Wait* (blocking), like the real workers.
class FakeSetupBackend final : public SetupBackend {
public:
    explicit FakeSetupBackend(AutostartTarget client = ce::runtime::ClientAutostartTarget(kClientExecutable, kClientArguments))
        : client(std::move(client)) {}

    AutostartTarget client;  // what ClientTarget() reports: the calling client's registration
    std::vector<std::string> calls;
    std::optional<AutostartTarget> autostartTarget;  // target of the last autostart request or query
    unsigned long elevationResult = kSuccess;
    unsigned long autostartResult = kSuccess;
    unsigned long driverResult = kSuccess;
    ElevationServiceState elevationState = ElevationServiceState::Absent;
    AutostartState autostartState = AutostartState::Absent;
    bool driverInstalled = false;
    bool launchElevated = false;
    bool elevationPreferred = false;
    bool libraryPresent = true;
    bool suppressed = false;
    unsigned long launchResult = kSuccess;
    int dismissCalls = 0;
    std::optional<unsigned long> pendingSetup;
    std::optional<unsigned long> pendingDriver;

    AutostartTarget ClientTarget() override { return client; }
    unsigned long RequestElevationService(bool install) override {
        calls.push_back(install ? "RequestElevationService(install)" : "RequestElevationService(remove)");
        return elevationResult;
    }
    unsigned long RequestClientAutostart(const AutostartTarget& target, bool enable) override {
        calls.push_back(enable ? "RequestClientAutostart(on)" : "RequestClientAutostart(off)");
        autostartTarget = target;
        return autostartResult;
    }
    std::optional<unsigned long> TakeSetupResult() override {
        calls.push_back("TakeSetupResult");
        return std::exchange(pendingSetup, std::nullopt);
    }
    std::optional<unsigned long> WaitSetupResult() override {
        calls.push_back("WaitSetupResult");
        return std::exchange(pendingSetup, std::nullopt);
    }
    ElevationServiceState QueryElevationServiceState() override {
        calls.push_back("QueryElevationServiceState");
        return elevationState;
    }
    AutostartState QueryClientAutostart(const AutostartTarget& target) override {
        calls.push_back("QueryClientAutostart");
        autostartTarget = target;
        return autostartState;
    }
    unsigned long RequestDriverInstall() override {
        calls.push_back("RequestDriverInstall");
        return driverResult;
    }
    unsigned long RequestDriverUninstall() override {
        calls.push_back("RequestDriverUninstall");
        return driverResult;
    }
    std::optional<unsigned long> TakeDriverSetupResult() override {
        calls.push_back("TakeDriverSetupResult");
        return std::exchange(pendingDriver, std::nullopt);
    }
    std::optional<unsigned long> WaitDriverSetupResult() override {
        calls.push_back("WaitDriverSetupResult");
        return std::exchange(pendingDriver, std::nullopt);
    }
    bool IsDriverInstalled() override {
        calls.push_back("IsDriverInstalled");
        return driverInstalled;
    }
    bool LaunchElevated() override { return launchElevated; }
    bool ElevationServicePreferred() override { return elevationPreferred; }
    bool SensorLibraryPresent() override { return libraryPresent; }
    bool SensorOfferSuppressed() override { return suppressed; }
    unsigned long RequestLaunchElevated(bool enable) override {
        calls.push_back(enable ? "RequestLaunchElevated(on)" : "RequestLaunchElevated(off)");
        return launchResult;
    }
    void DismissSensorOffer() override {
        calls.push_back("DismissSensorOffer");
        ++dismissCalls;
    }
};

// The registration name a client executable receives, read back from the autostart request it produces.
std::wstring RegisteredName(const std::wstring& executable) {
    FakeSetupBackend fake(ce::runtime::ClientAutostartTarget(executable, L""));
    SetupActions actions(fake);
    EXPECT_EQ(actions.Start(1, SetupAction::ClientAutostartOn), kSuccess);
    if (!fake.autostartTarget)
        return {};
    EXPECT_TRUE(ce::startup::IsValidAutostartTarget(*fake.autostartTarget));
    return fake.autostartTarget->name;
}

}  // namespace

TEST(SetupActionsComponentTest, EachActionReachesItsBackendCallWithTheRightArguments) {
    struct Case {
        SetupAction action;
        const char* call;
        bool autostart;
    };
    const Case cases[] = {
        {SetupAction::SensorDriverInstall, "RequestDriverInstall", false},
        {SetupAction::SensorDriverUninstall, "RequestDriverUninstall", false},
        {SetupAction::ElevationServiceInstall, "RequestElevationService(install)", false},
        {SetupAction::ElevationServiceRemove, "RequestElevationService(remove)", false},
        {SetupAction::ClientAutostartOn, "RequestClientAutostart(on)", true},
        {SetupAction::ClientAutostartOff, "RequestClientAutostart(off)", true},
        {SetupAction::LaunchElevatedOn, "RequestLaunchElevated(on)", false},
        {SetupAction::LaunchElevatedOff, "RequestLaunchElevated(off)", false},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.call);
        FakeSetupBackend fake;
        SetupActions actions(fake);
        EXPECT_EQ(actions.Start(9, test.action), kSuccess);
        EXPECT_EQ(fake.calls, std::vector<std::string>{test.call});
        EXPECT_TRUE(actions.Busy());
        if (test.autostart) {
            ASSERT_TRUE(fake.autostartTarget.has_value());
            EXPECT_EQ(fake.autostartTarget.value().executable, kClientExecutable);
            EXPECT_EQ(fake.autostartTarget.value().arguments, kClientArguments);
        } else {
            EXPECT_FALSE(fake.autostartTarget.has_value());
        }
    }
}

TEST(SetupActionsComponentTest, EachFamilyDeliversItsCompletionThroughItsOwnTakeFunction) {
    FakeSetupBackend fake;
    SetupActions actions(fake);

    ASSERT_EQ(actions.Start(1, SetupAction::SensorDriverInstall), kSuccess);
    EXPECT_TRUE(actions.Collect().empty());  // still running: nothing to deliver
    EXPECT_TRUE(actions.Busy());
    fake.calls.clear();
    fake.pendingDriver = kSuccess;
    const std::vector<SetupCompletion> driver = actions.Collect();
    EXPECT_EQ(fake.calls, std::vector<std::string>{"TakeDriverSetupResult"});
    ASSERT_EQ(driver.size(), 1u);
    EXPECT_EQ(driver[0].request, 1u);
    EXPECT_EQ(driver[0].win32Result, kSuccess);

    ASSERT_EQ(actions.Start(2, SetupAction::ElevationServiceRemove), kSuccess);
    fake.calls.clear();
    fake.pendingSetup = kCancelled;  // a declined UAC prompt is a result, not a refusal
    const std::vector<SetupCompletion> startup = actions.Collect();
    EXPECT_EQ(fake.calls, std::vector<std::string>{"TakeSetupResult"});
    ASSERT_EQ(startup.size(), 1u);
    EXPECT_EQ(startup[0].request, 2u);
    EXPECT_EQ(startup[0].win32Result, kCancelled);
}

TEST(SetupActionsComponentTest, OneActionAtATimeAcrossBothFamilies) {
    FakeSetupBackend fake;
    SetupActions actions(fake);

    ASSERT_EQ(actions.Start(1, SetupAction::SensorDriverInstall), kSuccess);
    fake.calls.clear();
    EXPECT_EQ(actions.Start(2, SetupAction::ElevationServiceInstall), kBusy);
    EXPECT_EQ(actions.Start(3, SetupAction::ClientAutostartOn), kBusy);
    EXPECT_EQ(actions.Start(4, SetupAction::SensorDriverUninstall), kBusy);
    EXPECT_TRUE(fake.calls.empty());  // refused starts never reach the backend
    EXPECT_TRUE(actions.Busy());

    fake.pendingDriver = kSuccess;
    ASSERT_EQ(actions.Collect().size(), 1u);
    EXPECT_FALSE(actions.Busy());

    // The other family may start once the first completion has been collected.
    EXPECT_EQ(actions.Start(5, SetupAction::ElevationServiceInstall), kSuccess);
    EXPECT_EQ(actions.Start(6, SetupAction::SensorDriverInstall), kBusy);
    fake.pendingSetup = kSuccess;
    ASSERT_EQ(actions.Collect().size(), 1u);
    EXPECT_FALSE(actions.Busy());
}

TEST(SetupActionsComponentTest, ABackendRefusalStartsNothingAndLeavesTheComponentIdle) {
    FakeSetupBackend fake;
    SetupActions actions(fake);

    fake.driverResult = kBusy;  // the PawnIO worker is busy with another request
    EXPECT_EQ(actions.Start(1, SetupAction::SensorDriverInstall), kBusy);
    EXPECT_FALSE(actions.Busy());
    EXPECT_TRUE(actions.Collect().empty());

    fake.autostartResult = kFileNotFound;  // the client executable is missing
    EXPECT_EQ(actions.Start(2, SetupAction::ClientAutostartOn), kFileNotFound);
    EXPECT_FALSE(actions.Busy());

    fake.driverResult = kSuccess;
    EXPECT_EQ(actions.Start(3, SetupAction::SensorDriverInstall), kSuccess);
    EXPECT_TRUE(actions.Busy());
}

TEST(SetupActionsComponentTest, CompletionIsDeliveredExactlyOnceWithItsRequestId) {
    FakeSetupBackend fake;
    SetupActions actions(fake);
    ASSERT_EQ(actions.Start(77, SetupAction::ElevationServiceInstall), kSuccess);

    fake.pendingSetup = kCancelled;
    const std::vector<SetupCompletion> first = actions.Collect();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].request, 77u);
    EXPECT_EQ(first[0].win32Result, kCancelled);
    EXPECT_TRUE(actions.Collect().empty());
    EXPECT_FALSE(actions.Busy());

    // The next action reports under its own id; the earlier result is not replayed.
    ASSERT_EQ(actions.Start(78, SetupAction::ElevationServiceRemove), kSuccess);
    fake.pendingSetup = kSuccess;
    const std::vector<SetupCompletion> second = actions.Collect();
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].request, 78u);
    EXPECT_EQ(second[0].win32Result, kSuccess);
}

TEST(SetupActionsComponentTest, UnknownActionsAreRejectedWithoutTouchingTheBackend) {
    FakeSetupBackend fake;
    SetupActions actions(fake);

    EXPECT_EQ(actions.Start(1, static_cast<SetupAction>(0)), kInvalidParameter);
    EXPECT_EQ(actions.Start(2, static_cast<SetupAction>(10)), kInvalidParameter);
    EXPECT_EQ(actions.Start(3, static_cast<SetupAction>(-1)), kInvalidParameter);
    EXPECT_TRUE(fake.calls.empty());
    EXPECT_FALSE(actions.Busy());
}

TEST(SetupActionsComponentTest, StatusComposesTheBackendAnswers) {
    struct AutostartCase {
        AutostartState state;
        bool clientAutostart;
    };
    const AutostartCase autostartCases[] = {
        {AutostartState::Absent, false},      {AutostartState::UserRun, true},
        {AutostartState::ElevatedTask, true}, {AutostartState::Mismatch, false},
        {AutostartState::Unknown, false},
    };

    FakeSetupBackend fake;
    SetupActions actions(fake);
    fake.driverInstalled = true;
    fake.elevationState = ElevationServiceState::Running;
    for (const AutostartCase& test : autostartCases) {
        SCOPED_TRACE(static_cast<int>(test.state));
        fake.autostartState = test.state;
        const SetupStatus status = actions.Status(HardwareSensorsConfig{});
        EXPECT_TRUE(status.sensorDriverInstalled);
        EXPECT_EQ(status.elevationServiceState, 2);
        EXPECT_EQ(status.clientAutostart, test.clientAutostart);
    }
    ASSERT_TRUE(fake.autostartTarget.has_value());
    EXPECT_EQ(fake.autostartTarget.value().executable, kClientExecutable);
    EXPECT_EQ(fake.autostartTarget.value().arguments, kClientArguments);

    fake.driverInstalled = false;
    fake.elevationState = ElevationServiceState::Installed;
    fake.autostartState = AutostartState::Absent;
    SetupStatus status = actions.Status(HardwareSensorsConfig{});
    EXPECT_FALSE(status.sensorDriverInstalled);
    EXPECT_EQ(status.elevationServiceState, 1);
    EXPECT_FALSE(status.clientAutostart);

    fake.elevationState = ElevationServiceState::Unknown;
    EXPECT_EQ(actions.Status(HardwareSensorsConfig{}).elevationServiceState, -1);
    EXPECT_FALSE(actions.Busy());
}

TEST(SetupActionsComponentTest, ShutdownWaitsForTheInFlightActionThroughItsWaitFunction) {
    FakeSetupBackend fake;
    SetupActions actions(fake);

    actions.Shutdown();  // idle: nothing to wait for
    EXPECT_TRUE(fake.calls.empty());

    ASSERT_EQ(actions.Start(5, SetupAction::SensorDriverUninstall), kSuccess);
    fake.calls.clear();
    fake.pendingDriver = kSuccess;  // the worker finishes while Shutdown waits for it
    const std::optional<SetupCompletion> driver = actions.Shutdown();
    EXPECT_EQ(fake.calls, std::vector<std::string>{"WaitDriverSetupResult"});
    EXPECT_FALSE(actions.Busy());
    // The wait hands back the real result, under the request id it was started with.
    ASSERT_TRUE(driver.has_value());
    EXPECT_EQ(driver.value().request, 5u);
    EXPECT_EQ(driver.value().win32Result, kSuccess);
    EXPECT_TRUE(actions.Collect().empty());  // the wait consumed the completion; nothing is delivered later
    EXPECT_EQ(fake.calls, std::vector<std::string>{"WaitDriverSetupResult"});

    ASSERT_EQ(actions.Start(6, SetupAction::ElevationServiceInstall), kSuccess);
    fake.calls.clear();
    fake.pendingSetup = kCancelled;
    const std::optional<SetupCompletion> startup = actions.Shutdown();
    EXPECT_EQ(fake.calls, std::vector<std::string>{"WaitSetupResult"});
    EXPECT_FALSE(actions.Busy());
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup.value().request, 6u);
    EXPECT_EQ(startup.value().win32Result, kCancelled);
}

TEST(SetupActionsComponentTest, ShutdownWithoutABackendResultReturnsNoCompletion) {
    FakeSetupBackend fake;
    SetupActions actions(fake);
    ASSERT_EQ(actions.Start(8, SetupAction::SensorDriverInstall), kSuccess);
    fake.pendingDriver = std::nullopt;  // the worker gave no result (it is being torn down)
    EXPECT_FALSE(actions.Shutdown().has_value());
    EXPECT_FALSE(actions.Busy());
}

TEST(SetupActionsComponentTest, KnownActionCodesAreTheNinePublicActions) {
    for (int32_t value = 1; value <= 9; ++value)
        EXPECT_TRUE(ce::runtime::IsKnownSetupAction(value)) << value;
    for (int32_t value : {-1, 0, 10, 100})
        EXPECT_FALSE(ce::runtime::IsKnownSetupAction(value)) << value;
}

TEST(SetupActionsComponentTest, TheDismissalIsNotAWorkerActionAndTheStatusCarriesTheOffer) {
    FakeSetupBackend fake;
    SetupActions actions(fake);
    // The engine completes the dismissal itself; an action the component is asked to start is refused, untouched.
    EXPECT_EQ(actions.Start(1, SetupAction::SensorOfferDismiss), kInvalidParameter);
    EXPECT_TRUE(fake.calls.empty());
    EXPECT_FALSE(actions.Busy());

    const HardwareSensorsConfig sensors;  // the defaults: CPU sensors on automatic
    fake.driverInstalled = false;
    EXPECT_TRUE(actions.Status(sensors).sensorDriverOffer);
    fake.suppressed = true;
    EXPECT_FALSE(actions.Status(sensors).sensorDriverOffer) << "declined";
    fake.suppressed = false;
    fake.libraryPresent = false;
    EXPECT_FALSE(actions.Status(sensors).sensorDriverOffer) << "no sensor library";
    fake.libraryPresent = true;
    fake.driverInstalled = true;
    EXPECT_FALSE(actions.Status(sensors).sensorDriverOffer) << "driver installed";

    fake.launchElevated = true;
    fake.elevationPreferred = true;
    const SetupStatus status = actions.Status(sensors);
    EXPECT_TRUE(status.launchElevated);
    EXPECT_TRUE(status.elevationServicePreferred);
}

TEST(SetupActionsComponentTest, ClientRegistrationIsValidStableAndNeverTheApplicationsOwn) {
    const std::wstring registered = RegisteredName(kClientExecutable);
    EXPECT_FALSE(registered.empty());
    EXPECT_NE(registered, ce::startup::kApplicationAutostartName);
    // The same executable names the same entry on every start. The entry follows the file name, so moving the client
    // replaces its Run value in place, while a differently named client gets its own entry.
    EXPECT_EQ(RegisteredName(kClientExecutable), registered);
    EXPECT_EQ(RegisteredName(LR"(D:\Moved\client.exe)"), registered);
    EXPECT_NE(RegisteredName(LR"(D:\Other\other-client.exe)"), registered);
    // A client that is itself named like the application must not take the application's registration name.
    EXPECT_NE(RegisteredName(LR"(D:\Games\CaptureEngine.exe)"), ce::startup::kApplicationAutostartName);
    // Characters outside the registration name alphabet are replaced, so the target stays valid.
    EXPECT_EQ(RegisteredName(LR"(C:\Apps\My Client (2).exe)"), L"Client-My_Client__2_");
}
