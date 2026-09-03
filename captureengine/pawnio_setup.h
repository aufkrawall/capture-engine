#pragma once

// Optional PawnIO driver setup for the LibreHardwareMonitor bridge.
//
// LibreHardwareMonitor 0.9.6 reads CPU temperature, package power and core
// clocks through PawnIO, a separately installed WHQL-signed kernel driver, and
// reports every one of those rails as exactly zero without it. CaptureEngine
// neither bundles nor downloads that driver: it detects whether the driver is
// present, and - only after the user says yes - hands the installation to the
// official package source under a single UAC prompt.

#include <windows.h>

#include <optional>

struct HardwareSensorsConfig;

namespace ce::pawnio {

// Elevated setup roles of captureengine.exe. Keeping them in the product
// executable rather than in a shipped script matters: a script beside the
// executable that installs a kernel driver under elevation is a local
// privilege-escalation vector for anyone who can write to that directory.
inline constexpr wchar_t kInstallCommand[] = L"--install-pawnio";
inline constexpr wchar_t kUninstallCommand[] = L"--uninstall-pawnio";

// Whether the PawnIO driver service is registered on this machine.
bool IsDriverInstalled();

// Whether the user answered "don't ask again" on a previous run.
bool IsPromptSuppressed();

// Offers installation on a dedicated UI thread and returns immediately. The
// controller's message loop dispatches hotkeys itself, so a modal dialog on
// that thread would swallow them for as long as it stayed open.
void OfferInstallationAsync(const ::HardwareSensorsConfig& config);

// Runs the elevated install/uninstall role when the command line selects it.
std::optional<int> TryRunPawnIoSetupHost();

}  // namespace ce::pawnio
