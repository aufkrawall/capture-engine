#include <gtest/gtest.h>
#include "captureengine/elevation/startup_control.h"
#include "common/ipc/elevation_windows.h"

using namespace ce::startup;

namespace {
AutostartTarget ClientTarget(const wchar_t* name = L"CeTestClient") {
    AutostartTarget target;
    target.executable = LR"(C:\Program Files\Client\client.exe)";
    target.arguments = L"--minimized";
    target.name = name;
    return target;
}
}  // namespace

TEST(AutostartTargetTest, TheApplicationRegistrationKeepsItsEstablishedShape) {
    const AutostartTarget application = ApplicationAutostartTarget();
    const std::wstring executable = ce::elevation::ExecutablePath();
    EXPECT_EQ(application.executable, executable);
    EXPECT_EQ(application.arguments, L"--autostart");
    EXPECT_EQ(application.name, L"CaptureEngine");
    EXPECT_TRUE(IsApplicationAutostart(application));
    // Registrations written by earlier versions must keep matching: same Run command, same task name.
    EXPECT_EQ(AutostartCommandLine(application), ce::elevation::QuoteArgument(executable) + L" --autostart");
    EXPECT_EQ(AutostartTaskName(application), L"CaptureEngine.Autostart." + OwnerSid());

    AutostartTarget other = application;
    other.name = L"OtherClient";
    EXPECT_FALSE(IsApplicationAutostart(other));
    other = application;
    other.arguments = L"--minimized";
    EXPECT_FALSE(IsApplicationAutostart(other));
    other = application;
    other.executable = LR"(C:\elsewhere\client.exe)";
    EXPECT_FALSE(IsApplicationAutostart(other));
    EXPECT_EQ(AutostartCommandLine(ClientTarget()), LR"("C:\Program Files\Client\client.exe" --minimized)");
    AutostartTarget bare = ClientTarget();
    bare.arguments.clear();
    EXPECT_EQ(AutostartCommandLine(bare), LR"("C:\Program Files\Client\client.exe")");
}

TEST(SetupRequestTest, RejectedClientAutostartTargetsStartNothing) {
    AutostartTarget relative = ClientTarget();
    relative.executable = L"client.exe";
    EXPECT_EQ(RequestClientAutostart(relative, true), ERROR_INVALID_PARAMETER);
    EXPECT_EQ(RequestClientAutostart(relative, false), ERROR_INVALID_PARAMETER);
    AutostartTarget badName = ClientTarget(L"not a name");
    EXPECT_EQ(RequestClientAutostart(badName, true), ERROR_INVALID_PARAMETER);
    // A well-formed target that does not exist cannot be registered to start at logon.
    AutostartTarget missing = ClientTarget();
    missing.executable = LR"(C:\ce-test-missing-directory\client.exe)";
    EXPECT_EQ(RequestClientAutostart(missing, true), ERROR_FILE_NOT_FOUND);
    EXPECT_FALSE(Busy());
    EXPECT_FALSE(TakeSetupResult().has_value());
    EXPECT_FALSE(WaitSetupResult().has_value());
}

TEST(SetupStatusTest, UnknownServicesAndUnregisteredClientsReadAsAbsentNeverAsPresent) {
    const ElevationServiceState service = QueryServiceState(L"CaptureEngineTestNoSuchService");
    EXPECT_TRUE(service == ElevationServiceState::Absent || service == ElevationServiceState::Unknown);
    const AutostartState client = QueryClientAutostart(ClientTarget(L"CeTestNeverRegistered"));
    EXPECT_TRUE(client == AutostartState::Absent || client == AutostartState::Unknown);
    AutostartTarget invalid = ClientTarget();
    invalid.executable = L"relative.exe";
    EXPECT_EQ(QueryClientAutostart(invalid), AutostartState::Unknown);
}

namespace {
// Removes the per-user registration a test made, also when an assertion returns early.
struct ScopedRunEntry {
    AutostartTarget target;
    ~ScopedRunEntry() {
        ConfigureClientAutostart(target, Registration::None);
    }
};

AutostartTarget UnitTestTarget(const wchar_t* name) {
    AutostartTarget target;
    target.executable = ce::elevation::ExecutablePath();
    target.arguments = L"--ce-unit-test-autostart";
    target.name = name;
    return target;
}

std::wstring RunValueOf(const AutostartTarget& target) {
    const std::wstring path = OwnerSid() + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    wchar_t value[4096]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_USERS, path.c_str(), target.name.c_str(), RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS)
        return L"<absent>";
    return value;
}
}  // namespace

TEST(SetupRequestTest, PlainClientAutostartRegistersAndRemovesAPerUserRunEntry) {
    const AutostartTarget target = UnitTestTarget(L"CeUnitTestAutostart");
    ScopedRunEntry cleanup{target};
    ASSERT_EQ(ConfigureClientAutostart(target, Registration::None), ERROR_SUCCESS);  // a leftover of an aborted run
    if (QueryClientAutostart(target) != AutostartState::Absent)
        GTEST_SKIP() << "the logon registrations cannot be read in this session";

    // Not the application's own registration: a plain Run entry that needs no elevation, no matter
    // which preferences the user stored for the application.
    ASSERT_FALSE(IsApplicationAutostart(target));
    ASSERT_EQ(RequestClientAutostart(target, true), ERROR_SUCCESS);
    EXPECT_EQ(WaitSetupResult().value_or(ERROR_INVALID_DATA), ERROR_SUCCESS);
    EXPECT_FALSE(Busy());
    EXPECT_EQ(QueryClientAutostart(target), AutostartState::UserRun);
    EXPECT_EQ(RunValueOf(target), AutostartCommandLine(target));

    ASSERT_EQ(RequestClientAutostart(target, true), ERROR_SUCCESS);  // already on: succeeds, nothing to do
    EXPECT_EQ(WaitSetupResult().value_or(ERROR_INVALID_DATA), ERROR_SUCCESS);
    EXPECT_EQ(QueryClientAutostart(target), AutostartState::UserRun);

    AutostartTarget moved = target;
    moved.arguments = L"--ce-unit-test-autostart --other";
    EXPECT_EQ(QueryClientAutostart(moved), AutostartState::Mismatch);  // same name, different command

    ASSERT_EQ(RequestClientAutostart(target, false), ERROR_SUCCESS);
    EXPECT_EQ(WaitSetupResult().value_or(ERROR_INVALID_DATA), ERROR_SUCCESS);
    EXPECT_EQ(QueryClientAutostart(target), AutostartState::Absent);
    EXPECT_EQ(RunValueOf(target), L"<absent>");
    ASSERT_EQ(RequestClientAutostart(target, false), ERROR_SUCCESS);  // already off
    EXPECT_EQ(WaitSetupResult().value_or(ERROR_INVALID_DATA), ERROR_SUCCESS);
}

TEST(SetupRequestTest, ASecondRequestIsRefusedWhileOneIsInFlight) {
    const AutostartTarget target = UnitTestTarget(L"CeUnitTestBusy");
    ScopedRunEntry cleanup{target};
    if (QueryClientAutostart(target) != AutostartState::Absent)
        GTEST_SKIP() << "the logon registrations cannot be read in this session";
    ASSERT_EQ(RequestClientAutostart(target, true), ERROR_SUCCESS);
    // The request stays busy until its result is collected, however fast the worker finished.
    EXPECT_TRUE(Busy());
    EXPECT_EQ(RequestClientAutostart(target, false), ERROR_BUSY);
    EXPECT_EQ(RequestElevationService(false), ERROR_BUSY);
    EXPECT_EQ(WaitSetupResult().value_or(ERROR_INVALID_DATA), ERROR_SUCCESS);
    EXPECT_FALSE(Busy());
}
