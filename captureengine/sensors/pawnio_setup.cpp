#include "pawnio_setup.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/logging/logging.h"
#include "sensor_selection_policy.h"
#include "pawnio_driver.h"
#include "pawnio_setup_core.h"
#include "pawnio_workers.h"

namespace ce::pawnio {
namespace {

bool RegistryKeyExists(HKEY root, const wchar_t* subKey, REGSAM extraAccess = 0) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | extraAccess, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD returned = 0;
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

// The prompt is pointless when the managed library the driver feeds is absent.
bool AreSensorLibraryFilesPresent() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return false;
    buffer.resize(length);
    const std::filesystem::path library = std::filesystem::path(buffer).parent_path() / L"plugins" /
                                          L"LibreHardwareMonitor" / L"LibreHardwareMonitorLib.dll";
    const DWORD attributes = GetFileAttributesW(library.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::filesystem::path ExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

void SuppressPrompt() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSuppressionKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        LogWarn("[PawnIO] Cannot record the prompt preference (error=%lu)", GetLastError());
        return;
    }
    const DWORD suppressed = 1;
    RegSetValueExW(key, kSuppressionValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&suppressed),
                   sizeof(suppressed));
    RegCloseKey(key);
    LogInfo("[PawnIO] Recorded the user's choice not to be asked about the driver again");
}

bool VerifyAuthenticode(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = path.c_str();

    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policyGuid, &trustData);

    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policyGuid, &trustData);

    return status == ERROR_SUCCESS;
}

// Relaunches this executable elevated for one setup action and waits for it. ERROR_SUCCESS with the child's exit
// code in *exitCode, or the Win32 status that stopped the launch (ERROR_CANCELLED when UAC was declined).
DWORD RunElevatedSetupRole(const wchar_t* command, int* exitCode) {
    const std::filesystem::path executable = ExecutablePath();
    if (executable.empty())
        return ERROR_GEN_FAILURE;
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = command;
    info.lpDirectory = executable.parent_path().c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS)
            error = ERROR_GEN_FAILURE;
        if (error == ERROR_CANCELLED)
            LogInfo("[PawnIO] The user declined the elevation prompt");
        else
            LogWarn("[PawnIO] Cannot start the elevated setup role (error=%lu)", error);
        return error;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    *exitCode = static_cast<int>(code);
    return ERROR_SUCCESS;
}

// The Windows operations behind the dialog-free decisions in pawnio_setup_core.cpp.
SetupOperations RealSetupOperations() {
    SetupOperations operations;
    operations.isInstalled = [] { return IsDriverInstalled(); };
    operations.isElevated = [] { return IsProcessElevated(); };
    operations.installInProcess = [] { return InstallDriverNow(); };
    operations.uninstallInProcess = [] { return UninstallDriverNow(); };
    operations.runElevatedRole = [](const wchar_t* command, int* exitCode) {
        return RunElevatedSetupRole(command, exitCode);
    };
    operations.clearSuppression = [] { ClearPromptSuppression(); };
    return operations;
}

}  // namespace

std::wstring ComputeFileSha256(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(file);
        return {};
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD returned = 0;
    std::wstring hexDigest;

    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                          &returned, 0) == 0) {
        std::vector<UCHAR> hashObject(objectSize);
        if (BCryptCreateHash(alg, &hash, hashObject.data(), objectSize, nullptr, 0, 0) == 0) {
            std::array<UCHAR, 65536> buffer;
            DWORD bytesRead = 0;
            bool readSuccess = true;
            while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) &&
                   bytesRead > 0) {
                if (BCryptHashData(hash, buffer.data(), bytesRead, 0) != 0) {
                    readSuccess = false;
                    break;
                }
            }
            if (readSuccess) {
                std::array<UCHAR, 32> digest;
                if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
                    constexpr wchar_t kHexChars[] = L"0123456789abcdef";
                    hexDigest.reserve(64);
                    for (UCHAR byte : digest) {
                        hexDigest.push_back(kHexChars[(byte >> 4) & 0x0F]);
                        hexDigest.push_back(kHexChars[byte & 0x0F]);
                    }
                }
            }
            BCryptDestroyHash(hash);
        }
    }

    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return hexDigest;
}

bool VerifyPawnIoSetupBinary(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        LogWarn("[PawnIO] Installer binary not found at %ls", path.c_str());
        return false;
    }
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize < 500000 || fileSize > 32000000) {
        LogWarn("[PawnIO] Installer binary size suspicious: %llu bytes", static_cast<unsigned long long>(fileSize));
        return false;
    }
    if (!VerifyAuthenticode(path)) {
        LogWarn("[PawnIO] Installer binary Authenticode signature verification failed: %ls", path.c_str());
        return false;
    }
    const std::wstring sha256 = ComputeFileSha256(path);
    if (_wcsicmp(sha256.c_str(), kPawnIoExpectedSha256) != 0) {
        LogWarn("[PawnIO] Installer binary SHA-256 mismatch (got %ls, expected %ls)", sha256.c_str(),
                kPawnIoExpectedSha256);
        return false;
    }
    return true;
}

std::filesystem::path BundledPawnIoSetupPath() {
    const std::filesystem::path executable = ExecutablePath();
    if (executable.empty())
        return {};
    return executable.parent_path() / L"plugins" / L"LibreHardwareMonitor" / L"PawnIO_setup.exe";
}

bool IsDriverInstalled() {
    return RegistryKeyExists(HKEY_LOCAL_MACHINE, kDriverServiceKey, KEY_WOW64_64KEY);
}

bool IsPromptSuppressed() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSuppressionKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool suppressed =
        RegQueryValueExW(key, kSuppressionValue, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD && value != 0;
    RegCloseKey(key);
    return suppressed;
}

void ClearPromptSuppression() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSuppressionKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kSuppressionValue);
        RegCloseKey(key);
        LogInfo("[PawnIO] Cleared the prompt suppression preference");
    }
}

bool SensorLibraryFilesPresent() {
    return AreSensorLibraryFilesPresent();
}

void DeclineDriverOffer() {
    SuppressPrompt();
}

unsigned long RequestDriverInstall() {
    return StartSilentSetupRequest([] { return RunSilentInstall(RealSetupOperations()); });
}

unsigned long RequestDriverUninstall() {
    return StartSilentSetupRequest([] { return RunSilentUninstall(RealSetupOperations()); });
}

std::optional<unsigned long> TakeDriverSetupResult() {
    return TakeSilentSetupResult();
}

std::optional<unsigned long> WaitDriverSetupResult() {
    return WaitSilentSetupResult();
}

std::optional<int> TryRunPawnIoSetupHost() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return std::nullopt;
    const bool install = argumentCount == 2 && wcscmp(arguments[1], kInstallCommand) == 0;
    const bool uninstall = argumentCount == 2 && wcscmp(arguments[1], kUninstallCommand) == 0;
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    if (!install && !uninstall)
        return std::nullopt;
    if (!IsProcessElevated()) {
        int exitCode = -1;
        if (RunElevatedSetupRole(install ? kInstallCommand : kUninstallCommand, &exitCode) != ERROR_SUCCESS)
            return -1;
        return exitCode;
    }
    return install ? InstallDriverNow() : UninstallDriverNow();
}

}  // namespace ce::pawnio
