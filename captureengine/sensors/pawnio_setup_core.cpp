#include "pawnio_setup_core.h"

#include <objbase.h>

#include <utility>

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

// The request thread joins a COM apartment: the shell launch of the elevated role expects one.
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComApartment() {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

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

DWORD RunSilentInstall(const SetupOperations& operations) {
    return RunDriverInstall(operations).error;
}

DWORD RunSilentUninstall(const SetupOperations& operations) {
    return RunDriverUninstall(operations).error;
}

DWORD StartSilentRequest(ce::startup::SetupWorker& worker, std::function<DWORD()> work) {
    if (worker.Busy()) {
        LogInfo("[PawnIO] Silent setup request refused: another PawnIO request is in flight");
        return ERROR_BUSY;
    }
    const bool started = worker.Start([work = std::move(work)]() -> DWORD {
        ComApartment apartment;
        return work();
    });
    if (!started)
        return ERROR_BUSY;
    LogInfo("[PawnIO] Silent setup request started");
    return ERROR_SUCCESS;
}

}  // namespace ce::pawnio
