#pragma once

#include "startup_preferences.h"
#include <optional>

namespace ce::startup {
enum class Setting { Service, Elevation, Autostart };
std::optional<int> Bootstrap(bool controller);
std::optional<int> TryRunSetup();
void Toggle(Setting setting);
void Pump();
void Shutdown();
bool Busy();
Preferences DisplayPreferences();
bool AutostartMatches(const Preferences& preferences, bool administratorAccount);
std::wstring ServiceStatusText();
DWORD RunElevationIntegration();
DWORD RunElevationFixture(bool waitForLoss);
DWORD RunIntegrationSetup(bool install, const Preferences& preferences);
DWORD ConfigureAutostart(const Preferences& preferences, bool administratorAccount);
DWORD InstallElevationService();
DWORD ValidateSensorRuntime(const std::wstring& directory);
DWORD RemoveElevationService();
DWORD WaitElevationServiceRemoved();
}  // namespace ce::startup
