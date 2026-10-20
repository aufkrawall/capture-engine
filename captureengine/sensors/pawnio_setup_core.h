#pragma once

// Dialog-free PawnIO setup decisions. The tray's interactive install/uninstall and the silent setup requests
// run the same functions here; the Windows operations are injected, so the decisions are tested with fakes.
// Nothing in this header or in pawnio_setup_core.cpp shows UI.

#include <windows.h>

#include <functional>

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

// The tray's dialogs, injected so the interactive flow runs under test with recording fakes.
struct InstallDialogs {
    std::function<void()> alreadyInstalled;
    std::function<void()> installed;  // the change applied: the restart-as-administrator offer
    std::function<void()> notCompleted;
};
struct UninstallDialogs {
    std::function<void()> notInstalled;
    std::function<bool()> confirm;  // true when the user confirmed the removal
    std::function<void()> removed;
    std::function<void()> notCompleted;
};

// The interactive flows: the same decisions as the silent requests, with the dialogs shown around them.
void RunInteractiveInstall(const SetupOperations& operations, const InstallDialogs& dialogs);
void RunInteractiveUninstall(const SetupOperations& operations, const UninstallDialogs& dialogs);

}  // namespace ce::pawnio
