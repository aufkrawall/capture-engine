#pragma once

// Optional PawnIO driver setup for the LibreHardwareMonitor bridge.
//
// LibreHardwareMonitor 0.9.6 reads CPU temperature, package power and core
// clocks through PawnIO, a separately installed WHQL-signed kernel driver, and
// reports every one of those rails as exactly zero without it. CaptureEngine
// bundles the official Microsoft-signed installer for optional offline setup.
// Installation and uninstallation can be managed via the system tray, startup
// prompt, or command line.

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

struct HardwareSensorsConfig;

namespace ce::pawnio {

// Elevated setup roles of captureengine.exe.
inline constexpr wchar_t kInstallCommand[] = L"--install-pawnio";
inline constexpr wchar_t kUninstallCommand[] = L"--uninstall-pawnio";

inline constexpr wchar_t kDriverServiceKey[] = L"SYSTEM\\CurrentControlSet\\Services\\PawnIO";
inline constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";
inline constexpr wchar_t kSuppressionKey[] = L"Software\\CaptureEngine";
inline constexpr wchar_t kSuppressionValue[] = L"PawnIoPromptSuppressed";
inline constexpr wchar_t kPawnIoExpectedSha256[] = L"1f519a22e47187f70a1379a48ca604981c4fcf694f4e65b734aaa74a9fba3032";

// Whether the PawnIO driver service is registered on this machine.
bool IsDriverInstalled();

// Whether the user answered "don't ask again" on a previous run.
bool IsPromptSuppressed();

// Clears the user's suppression choice so prompts can be offered again.
void ClearPromptSuppression();

// Offers installation on a dedicated UI thread and returns immediately. The
// controller's message loop dispatches hotkeys itself, so a modal dialog on
// that thread would swallow them for as long as it stayed open.
void OfferInstallationAsync(const ::HardwareSensorsConfig& config);

// Installs the driver asynchronously on a dedicated UI thread.
void InstallDriverAsync();

// Uninstalls the driver asynchronously on a dedicated UI thread (prompts confirmation first).
void UninstallDriverAsync();

// Runs the elevated install/uninstall role when the command line selects it.
std::optional<int> TryRunPawnIoSetupHost();

// Verifies the bundled installer binary (size, Authenticode signature, SHA-256).
bool VerifyPawnIoSetupBinary(const std::filesystem::path& path);

// Computes the SHA-256 hex digest of a file.
std::wstring ComputeFileSha256(const std::filesystem::path& path);

// Path to the bundled installer binary.
std::filesystem::path BundledPawnIoSetupPath();

}  // namespace ce::pawnio
