#include "pawnio_setup_core.h"

#include "common/logging/logging.h"
#include "pawnio_setup.h"

namespace ce::pawnio {
namespace {

// The change ran in place (elevated) or through the elevated role. ERROR_SUCCESS with *exitCode set, or the Win32
// status that stopped the launch; ERROR_CANCELLED means the user declined the UAC prompt.
DWORD RunChange(const SetupOperations& operations, const wchar_t* command, const std::function<int()>& inProcess,
                int* exitCode) {
    if (operations.isElevated()) {
        *exitCode = inProcess();
        return ERROR_SUCCESS;
    }
    return operations.runElevatedRole(command, exitCode);
}

// reached: the requested state holds after a run that exited with code 0.
SetupResult Concluded(DWORD launch, bool reached) {
    if (launch == ERROR_CANCELLED)
        return {SetupOutcome::Declined, ERROR_CANCELLED};
    if (launch != ERROR_SUCCESS)
        return {SetupOutcome::Failed, launch};
    if (!reached)
        return {SetupOutcome::Failed, ERROR_GEN_FAILURE};
    return {SetupOutcome::Applied, ERROR_SUCCESS};
}

const char* OutcomeName(SetupOutcome outcome) {
    switch (outcome) {
        case SetupOutcome::AlreadyApplied:
            return "already-applied";
        case SetupOutcome::Applied:
            return "applied";
        case SetupOutcome::Declined:
            return "declined";
        case SetupOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

SetupResult Logged(const char* action, SetupResult result) {
    LogInfo("[PawnIO] Driver %s finished: outcome=%s error=%lu", action, OutcomeName(result.outcome), result.error);
    return result;
}

}  // namespace

SetupResult RunDriverInstall(const SetupOperations& operations) {
    if (operations.isInstalled())
        return {SetupOutcome::AlreadyApplied, ERROR_SUCCESS};
    int exitCode = -1;
    const DWORD launch = RunChange(operations, kInstallCommand, operations.installInProcess, &exitCode);
    const SetupResult result = Concluded(launch, exitCode == 0 && operations.isInstalled());
    if (result.outcome == SetupOutcome::Applied)
        operations.clearSuppression();
    return Logged("install", result);
}

SetupResult RunDriverUninstall(const SetupOperations& operations) {
    if (!operations.isInstalled())
        return {SetupOutcome::AlreadyApplied, ERROR_SUCCESS};
    int exitCode = -1;
    const DWORD launch = RunChange(operations, kUninstallCommand, operations.uninstallInProcess, &exitCode);
    return Logged("uninstall", Concluded(launch, exitCode == 0 && !operations.isInstalled()));
}

void RunInteractiveInstall(const SetupOperations& operations, const InstallDialogs& dialogs) {
    const SetupOutcome outcome = RunDriverInstall(operations).outcome;
    if (outcome == SetupOutcome::AlreadyApplied)
        dialogs.alreadyInstalled();
    else if (outcome == SetupOutcome::Applied)
        dialogs.installed();
    else
        dialogs.notCompleted();
}

void RunInteractiveUninstall(const SetupOperations& operations, const UninstallDialogs& dialogs) {
    if (!operations.isInstalled()) {
        dialogs.notInstalled();
        return;
    }
    if (!dialogs.confirm())
        return;
    const SetupOutcome outcome = RunDriverUninstall(operations).outcome;
    if (outcome == SetupOutcome::Applied || outcome == SetupOutcome::AlreadyApplied)
        dialogs.removed();
    else
        dialogs.notCompleted();
}

}  // namespace ce::pawnio
