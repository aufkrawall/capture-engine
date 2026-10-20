#pragma once

// Optional PawnIO driver setup for the LibreHardwareMonitor bridge.
//
// LibreHardwareMonitor 0.9.6 reads CPU temperature, package power and core
// clocks through PawnIO, a separately installed WHQL-signed kernel driver, and
// reports every one of those rails as exactly zero without it. CaptureEngine
// bundles the official Microsoft-signed installer for optional offline setup.
// Installation and uninstallation are requested through the setup API; the
// elevated command-line roles do the work.

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

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

// Whether the sensor library files are installed next to CaptureEngine (the driver's only consumer).
bool SensorLibraryFilesPresent();

// Records the user's "don't ask again" choice for the driver offer (the startup prompt's third answer).
void DeclineDriverOffer();

// Clears the user's suppression choice so prompts can be offered again.
void ClearPromptSuppression();

// Setup requests for callers that must not show UI (the library runtime). No dialog or message box is shown;
// the UAC prompt is the only UI, and it is unavoidable. Each request runs on one shared worker and returns at once:
// ERROR_SUCCESS when started, ERROR_BUSY while a PawnIO request is in flight or its result has not been collected.
// Installing a present driver or uninstalling an absent one completes with ERROR_SUCCESS without elevation; a declined
// UAC prompt completes with ERROR_CANCELLED.
// The caller must have stopped the sensor process before RequestDriverUninstall.
unsigned long RequestDriverInstall();
unsigned long RequestDriverUninstall();
// The finished request's Win32 status, delivered once: polled, or waited for until the request finishes.
std::optional<unsigned long> TakeDriverSetupResult();
std::optional<unsigned long> WaitDriverSetupResult();

// Runs the elevated install/uninstall role when the command line selects it.
std::optional<int> TryRunPawnIoSetupHost();

// Verifies the bundled installer binary (size, Authenticode signature, SHA-256).
bool VerifyPawnIoSetupBinary(const std::filesystem::path& path);

// Computes the SHA-256 hex digest of a file.
std::wstring ComputeFileSha256(const std::filesystem::path& path);

// Path to the bundled installer binary.
std::filesystem::path BundledPawnIoSetupPath();

}  // namespace ce::pawnio
