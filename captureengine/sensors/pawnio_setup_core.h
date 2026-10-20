#pragma once

// Dialog-free PawnIO setup decisions. The silent setup requests run these functions; the Windows operations are
// injected, so the decisions are tested with fakes. Nothing in this header or in pawnio_setup_core.cpp shows UI.

#include <windows.h>

#include <functional>

#include "captureengine/elevation/setup_worker.h"

namespace ce::pawnio {

// The OS operations behind the decisions. pawnio_setup.cpp binds the real ones; tests bind fakes.
struct SetupOperations {
    std::function<bool()> isInstalled;        // the PawnIO service is registered
    std::function<bool()> isElevated;         // this process runs elevated
    std::function<int()> installInProcess;    // bundled installer, elevated process; 0 on success
    std::function<int()> uninstallInProcess;  // full removal, elevated process; 0 when the driver is gone
    // Runs the elevated setup role for `command`. ERROR_SUCCESS with the child's exit code in *exitCode, or the
    // Win32 status that stopped the launch (ERROR_CANCELLED when the user declined UAC).
    std::function<DWORD(const wchar_t* command, int* exitCode)> runElevatedRole;
    std::function<void()> clearSuppression;  // clears the "don't ask again" choice after an install
};

enum class SetupOutcome {
    AlreadyApplied,  // the driver was already in the requested state; nothing was launched
    Applied,         // the change ran and the requested state is verified
    Declined,        // the user declined the elevation prompt
    Failed,          // the change could not start, or it did not reach the requested state
};

struct SetupResult {
    SetupOutcome outcome = SetupOutcome::Failed;
    DWORD error = ERROR_GEN_FAILURE;  // ERROR_SUCCESS, ERROR_CANCELLED, the launch error or ERROR_GEN_FAILURE
};

// Installs the driver unless it is present. Elevates through the setup role when this process is not elevated.
SetupResult RunDriverInstall(const SetupOperations& operations);
// Removes the driver unless it is absent. Same elevation rule.
SetupResult RunDriverUninstall(const SetupOperations& operations);

// Silent requests: the Win32 status of the finished change (ERROR_SUCCESS, ERROR_CANCELLED or the failure).
DWORD RunSilentInstall(const SetupOperations& operations);
DWORD RunSilentUninstall(const SetupOperations& operations);

// Starts a silent request on `worker`. Returns ERROR_SUCCESS when started, or ERROR_BUSY while a request is in flight
// or its result has not been collected. The work runs on the worker thread in a COM apartment; its result is collected
// once with worker.TakeResult() or worker.WaitForResult().
DWORD StartSilentRequest(ce::startup::SetupWorker& worker, std::function<DWORD()> work);

}  // namespace ce::pawnio
