#include <gtest/gtest.h>

#include <future>
#include <string>

#include "captureengine/elevation/setup_worker.h"
#include "captureengine/sensors/pawnio_setup.h"
#include "captureengine/sensors/pawnio_setup_core.h"

using ce::pawnio::SetupOperations;
using ce::pawnio::SetupOutcome;

namespace {

// Recording stand-in for the Windows operations. The driver state changes only when applyEffects is set, so a run can
// report success without the driver actually moving (the "did not reach the requested state" case).
struct FakeSetup {
    bool installed = false;
    bool elevated = false;
    bool applyEffects = true;
    int inProcessExit = 0;              // exit code of the in-process install or uninstall
    DWORD roleLaunch = ERROR_SUCCESS;   // launch status of the elevated role
    int roleExit = 0;                   // exit code of the elevated role
    int installCalls = 0;
    int uninstallCalls = 0;
    int roleCalls = 0;
    int suppressionCleared = 0;
    std::wstring lastCommand;

    SetupOperations Operations() {
        SetupOperations operations;
        operations.isInstalled = [this] { return installed; };
        operations.isElevated = [this] { return elevated; };
        operations.installInProcess = [this] {
            ++installCalls;
            if (applyEffects && inProcessExit == 0)
                installed = true;
            return inProcessExit;
        };
        operations.uninstallInProcess = [this] {
            ++uninstallCalls;
            if (applyEffects && inProcessExit == 0)
                installed = false;
            return inProcessExit;
        };
        operations.runElevatedRole = [this](const wchar_t* command, int* exitCode) -> DWORD {
            ++roleCalls;
            lastCommand = command;
            if (roleLaunch != ERROR_SUCCESS)
                return roleLaunch;
            *exitCode = roleExit;
            if (applyEffects && roleExit == 0)
                installed = lastCommand == ce::pawnio::kInstallCommand;
            return ERROR_SUCCESS;
        };
        operations.clearSuppression = [this] { ++suppressionCleared; };
        return operations;
    }
};

}  // namespace

TEST(PawnIoSilentSetupTest, InstallOfAPresentDriverCompletesWithoutLaunchingAnything) {
    FakeSetup fake;
    fake.installed = true;
    const ce::pawnio::SetupResult result = ce::pawnio::RunDriverInstall(fake.Operations());
    EXPECT_EQ(result.outcome, SetupOutcome::AlreadyApplied);
    EXPECT_EQ(result.error, ERROR_SUCCESS);
    EXPECT_EQ(fake.roleCalls + fake.installCalls, 0);
    EXPECT_EQ(fake.suppressionCleared, 0);
}

TEST(PawnIoSilentSetupTest, UninstallOfAnAbsentDriverCompletesWithoutLaunchingAnything) {
    FakeSetup fake;
    const ce::pawnio::SetupResult result = ce::pawnio::RunDriverUninstall(fake.Operations());
    EXPECT_EQ(result.outcome, SetupOutcome::AlreadyApplied);
    EXPECT_EQ(result.error, ERROR_SUCCESS);
    EXPECT_EQ(fake.roleCalls + fake.uninstallCalls, 0);
}

TEST(PawnIoSilentSetupTest, ElevatedRoleInstallAppliesAndClearsThePromptChoice) {
    FakeSetup fake;
    const ce::pawnio::SetupResult result = ce::pawnio::RunDriverInstall(fake.Operations());
    EXPECT_EQ(result.outcome, SetupOutcome::Applied);
    EXPECT_EQ(result.error, ERROR_SUCCESS);
    EXPECT_EQ(fake.roleCalls, 1);
    EXPECT_EQ(fake.lastCommand, std::wstring(ce::pawnio::kInstallCommand));
    EXPECT_EQ(fake.installCalls, 0);
    EXPECT_EQ(fake.suppressionCleared, 1);
}

TEST(PawnIoSilentSetupTest, ElevatedProcessInstallsInPlaceWithoutTheRole) {
    FakeSetup fake;
    fake.elevated = true;
    const ce::pawnio::SetupResult result = ce::pawnio::RunDriverInstall(fake.Operations());
    EXPECT_EQ(result.outcome, SetupOutcome::Applied);
    EXPECT_EQ(fake.installCalls, 1);
    EXPECT_EQ(fake.roleCalls, 0);
}

TEST(PawnIoSilentSetupTest, DeclinedUacPromptMapsToErrorCancelledAndChangesNothing) {
    FakeSetup fake;
    fake.roleLaunch = ERROR_CANCELLED;
    const ce::pawnio::SetupResult install = ce::pawnio::RunDriverInstall(fake.Operations());
    EXPECT_EQ(install.outcome, SetupOutcome::Declined);
    EXPECT_EQ(install.error, ERROR_CANCELLED);
    EXPECT_FALSE(fake.installed);
    EXPECT_EQ(fake.suppressionCleared, 0);

    fake.installed = true;
    const ce::pawnio::SetupResult uninstall = ce::pawnio::RunDriverUninstall(fake.Operations());
    EXPECT_EQ(uninstall.outcome, SetupOutcome::Declined);
    EXPECT_EQ(uninstall.error, ERROR_CANCELLED);
    EXPECT_TRUE(fake.installed);
}

TEST(PawnIoSilentSetupTest, LaunchFailureKeepsItsWin32Status) {
    FakeSetup fake;
    fake.roleLaunch = ERROR_FILE_NOT_FOUND;
    const ce::pawnio::SetupResult result = ce::pawnio::RunDriverInstall(fake.Operations());
    EXPECT_EQ(result.outcome, SetupOutcome::Failed);
    EXPECT_EQ(result.error, ERROR_FILE_NOT_FOUND);
}

TEST(PawnIoSilentSetupTest, FailedExitOrUnchangedDriverIsAFailure) {
    FakeSetup nonZeroExit;
    nonZeroExit.roleExit = 1;
    const ce::pawnio::SetupResult exited = ce::pawnio::RunDriverInstall(nonZeroExit.Operations());
    EXPECT_EQ(exited.outcome, SetupOutcome::Failed);
    EXPECT_EQ(exited.error, ERROR_GEN_FAILURE);
    EXPECT_EQ(nonZeroExit.suppressionCleared, 0);

    FakeSetup unchanged;
    unchanged.applyEffects = false;  // exit code 0, but the driver is still absent
    const ce::pawnio::SetupResult missing = ce::pawnio::RunDriverInstall(unchanged.Operations());
    EXPECT_EQ(missing.outcome, SetupOutcome::Failed);
    EXPECT_EQ(missing.error, ERROR_GEN_FAILURE);
    EXPECT_EQ(unchanged.suppressionCleared, 0);

    FakeSetup stillPresent;
    stillPresent.installed = true;
    stillPresent.elevated = true;
    stillPresent.applyEffects = false;
    stillPresent.inProcessExit = 1;
    const ce::pawnio::SetupResult uninstall = ce::pawnio::RunDriverUninstall(stillPresent.Operations());
    EXPECT_EQ(uninstall.outcome, SetupOutcome::Failed);
    EXPECT_EQ(uninstall.error, ERROR_GEN_FAILURE);
}

TEST(PawnIoSilentSetupTest, SilentRequestRefusesASecondRequestUntilTheFirstResultIsCollectedOnce) {
    ce::startup::SetupWorker worker;
    std::promise<void> running;
    std::promise<void> release;
    const std::shared_future<void> gate = release.get_future().share();
    ASSERT_EQ(ce::pawnio::StartSilentRequest(worker,
                                             [&running, gate]() -> DWORD {
                                                 running.set_value();
                                                 gate.wait();
                                                 return ERROR_SUCCESS;
                                             }),
              ERROR_SUCCESS);
    running.get_future().wait();
    bool secondRan = false;
    EXPECT_EQ(ce::pawnio::StartSilentRequest(worker,
                                             [&secondRan]() -> DWORD {
                                                 secondRan = true;
                                                 return ERROR_SUCCESS;
                                             }),
              ERROR_BUSY);
    EXPECT_FALSE(worker.TakeResult().has_value());  // still running: nothing to collect
    release.set_value();
    EXPECT_EQ(worker.WaitForResult().value_or(0), static_cast<unsigned long>(ERROR_SUCCESS));
    EXPECT_FALSE(secondRan);
    EXPECT_FALSE(worker.Busy());
    EXPECT_FALSE(worker.WaitForResult().has_value());  // delivered once
    EXPECT_FALSE(worker.TakeResult().has_value());
}

TEST(PawnIoSilentSetupTest, SilentRequestsReportTheDecisionAsAWin32Status) {
    FakeSetup present;
    present.installed = true;
    FakeSetup declined;
    declined.roleLaunch = ERROR_CANCELLED;
    const SetupOperations presentOperations = present.Operations();
    const SetupOperations declinedOperations = declined.Operations();

    ce::startup::SetupWorker worker;
    ASSERT_EQ(ce::pawnio::StartSilentRequest(worker,
                                             [presentOperations]() -> DWORD {
                                                 return ce::pawnio::RunSilentInstall(presentOperations);
                                             }),
              ERROR_SUCCESS);
    EXPECT_EQ(worker.WaitForResult().value_or(99), static_cast<unsigned long>(ERROR_SUCCESS));
    EXPECT_EQ(present.roleCalls, 0);

    ASSERT_EQ(ce::pawnio::StartSilentRequest(worker,
                                             [declinedOperations]() -> DWORD {
                                                 return ce::pawnio::RunSilentInstall(declinedOperations);
                                             }),
              ERROR_SUCCESS);
    EXPECT_EQ(worker.WaitForResult().value_or(99), static_cast<unsigned long>(ERROR_CANCELLED));
    EXPECT_FALSE(declined.installed);
}
