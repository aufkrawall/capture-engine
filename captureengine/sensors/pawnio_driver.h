#pragma once

// Driver work of the PawnIO setup, run in an elevated process: installs the bundled driver or removes it. No UI here;
// the callers decide what to show.

namespace ce::pawnio {

// Installs the driver with the verified bundled installer. Returns 0 on success, 1 otherwise.
int InstallDriverNow();

// Removes the driver: registered uninstaller, bundled installer, service, driver packages and leftovers. Returns 0
// when the driver is gone, 1 otherwise.
int UninstallDriverNow();

}  // namespace ce::pawnio
