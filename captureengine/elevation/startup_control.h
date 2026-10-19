#pragma once

#include "startup_preferences.h"
#include <optional>

namespace ce::startup {
// ---- Application policy (frontend today): relaunch elevated, tray toggles -------------------
std::optional<int> Bootstrap(bool controller);
std::optional<int> TryRunSetup();
// Elevated role run by the installer: service and startup registration for a named user.
std::optional<int> TryRunInstallerSetup();
void Toggle(Setting setting);
// Collects a finished tray request: logs it and reports a failure in a message box.
void Pump();
void Shutdown();
bool Busy();
Preferences DisplayPreferences();
bool AutostartMatches(const Preferences& preferences, bool administratorAccount);
std::wstring ServiceStatusText();

// ---- Setup actions: independently requestable, each with a status query ---------------------
// Requests run asynchronously on one shared setup worker (Busy() spans request to result) and
// return at once: ERROR_SUCCESS when started, ERROR_BUSY while another request is in flight,
// ERROR_INVALID_PARAMETER / ERROR_FILE_NOT_FOUND for a rejected target. The finished request's
// Win32 result is collected once with TakeSetupResult() (polling) or WaitSetupResult() (blocking).
// UAC is requested only when the change needs it; cancellation arrives as ERROR_CANCELLED.
DWORD RequestElevationService(bool install);
// Registers the calling client (not a fixed executable) to start at logon. The application's own
// registration follows its elevation preference and is persisted with it; any other client gets a
// plain per-user Run entry, which needs no elevation.
DWORD RequestClientAutostart(const AutostartTarget& target, bool enable);
std::optional<unsigned long> TakeSetupResult();
std::optional<unsigned long> WaitSetupResult();
ElevationServiceState QueryElevationServiceState();
ElevationServiceState QueryServiceState(const wchar_t* serviceName);
AutostartState QueryClientAutostart(const AutostartTarget& target);
AutostartTarget ApplicationAutostartTarget();
bool IsApplicationAutostart(const AutostartTarget& target);
// Exactly what is written to the Run key and named in the logon task for a target.
std::wstring AutostartCommandLine(const AutostartTarget& target);
std::wstring AutostartTaskName(const AutostartTarget& target);

DWORD RunElevationIntegration();
DWORD RunElevationFixture(bool waitForLoss);
DWORD RunIntegrationSetup(bool install, const Preferences& preferences);
DWORD ConfigureAutostart(const Preferences& preferences, bool administratorAccount);
DWORD ConfigureClientAutostart(const AutostartTarget& target, Registration mode);
DWORD InstallElevationService();
DWORD ValidateSensorRuntime(const std::wstring& directory);
DWORD RemoveElevationService();
DWORD WaitElevationServiceRemoved();
}  // namespace ce::startup
