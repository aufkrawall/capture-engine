#include "pawnio_setup.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../common/config.h"
#include "../common/logging.h"
#include "sensor_selection_policy.h"

namespace ce::pawnio {
namespace {

constexpr int kInstallButtonId = 101;
constexpr int kNotNowButtonId = 102;
constexpr int kNeverButtonId = 103;

bool RegistryKeyExists(HKEY root, const wchar_t* subKey, REGSAM extraAccess = 0) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | extraAccess, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring value;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t) && bytes <= 8192) {
        value.resize(bytes / sizeof(wchar_t));
        if (RegQueryValueExW(key, valueName, nullptr, nullptr, reinterpret_cast<BYTE*>(value.data()), &bytes) !=
            ERROR_SUCCESS) {
            value.clear();
        }
        while (!value.empty() && value.back() == L'\0')
            value.pop_back();
    }
    RegCloseKey(key);
    return value;
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

int RunProcessAndWait(const std::wstring& executable, std::wstring commandLine) {
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable.empty() ? nullptr : executable.c_str(), commandLine.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        LogWarn("[PawnIO] Cannot start %ls (error=%lu)", executable.c_str(), GetLastError());
        return -1;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 10 * 60 * 1000);
    DWORD exitCode = static_cast<DWORD>(-1);
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &exitCode);
    else
        TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
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

int InstallDriverNow() {
    const std::filesystem::path setupPath = BundledPawnIoSetupPath();
    if (!VerifyPawnIoSetupBinary(setupPath)) {
        LogWarn("[PawnIO] Bundled installer verification failed; cannot install driver: %ls", setupPath.c_str());
        return 1;
    }
    std::wstring commandLine = L"\"" + setupPath.wstring() + L"\" -install -silent";
    LogInfo("[PawnIO] Installing driver using bundled installer: %ls", setupPath.c_str());
    const int result = RunProcessAndWait(setupPath.wstring(), commandLine);
    LogInfo("[PawnIO] Bundled installer finished with exit code %d", result);
    return (result == 0 || result == 3010) ? 0 : 1;
}

int UninstallDriverNow() {
    std::wstring command = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"QuietUninstallString");
    if (command.empty())
        command = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString");
    if (!command.empty()) {
        LogInfo("[PawnIO] Running registered uninstall command: %ls", command.c_str());
        const int result = RunProcessAndWait(std::wstring(), command);
        LogInfo("[PawnIO] Registered uninstaller finished with exit code %d", result);
        if (result == 0 || result == 3010)
            return 0;
    }

    const std::filesystem::path setupPath = BundledPawnIoSetupPath();
    if (VerifyPawnIoSetupBinary(setupPath)) {
        std::wstring commandLine = L"\"" + setupPath.wstring() + L"\" -uninstall -silent";
        LogInfo("[PawnIO] Running uninstallation via bundled installer: %ls", setupPath.c_str());
        const int result = RunProcessAndWait(setupPath.wstring(), commandLine);
        LogInfo("[PawnIO] Bundled uninstaller finished with exit code %d", result);
        return (result == 0 || result == 3010) ? 0 : 1;
    }

    LogWarn("[PawnIO] No registered uninstaller or valid bundled installer found");
    return 2;
}

// Relaunches this executable elevated for one setup action. Returns the child's
// exit code, or -1 when the user declined the UAC prompt.
int RunElevatedSetupRole(const wchar_t* command) {
    const std::filesystem::path executable = ExecutablePath();
    if (executable.empty())
        return -1;
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = command;
    info.lpDirectory = executable.parent_path().c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED)
            LogInfo("[PawnIO] The user declined the elevation prompt");
        else
            LogWarn("[PawnIO] Cannot start the elevated setup role (error=%lu)", error);
        return -1;
    }
    WaitForSingleObject(info.hProcess, 10 * 60 * 1000);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    return static_cast<int>(exitCode);
}

typedef HRESULT(WINAPI* TaskDialogIndirectFunction)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

void RestartAsAdministrator() {
    const std::filesystem::path executable = ExecutablePath();
    if (executable.empty())
        return;
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpDirectory = executable.parent_path().c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        LogInfo("[PawnIO] Spawning elevated CaptureEngine and exiting unelevated instance");
        HWND trayHWnd = FindWindowA("CaptureEngineTray", nullptr);
        if (trayHWnd) {
            PostMessageA(trayHWnd, WM_CLOSE, 0, 0);
        } else {
            ExitProcess(0);
        }
    }
}

void OfferRestartAsAdministrator() {
    if (IsProcessElevated()) {
        MessageBoxW(nullptr,
                    L"PawnIO was installed successfully.\n\n"
                    L"CPU temperature, package power and core clocks are now available.",
                    L"CaptureEngine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }

    HMODULE controls = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto taskDialog = controls ? reinterpret_cast<TaskDialogIndirectFunction>(
                                     GetProcAddress(controls, "TaskDialogIndirect"))
                               : nullptr;

    constexpr int kRestartButtonId = 201;
    constexpr int kLaterButtonId = 202;

    int choice = 0;
    if (taskDialog) {
        const TASKDIALOG_BUTTON buttons[] = {
            {kRestartButtonId,
             L"Restart as Administrator now\nCloses CaptureEngine and relaunches it elevated so CPU sensors work immediately."},
            {kLaterButtonId,
             L"Later\nKeep running without elevation. CPU sensors will remain unavailable until restarted as admin."},
        };
        TASKDIALOGCONFIG config = {};
        config.cbSize = sizeof(config);
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        config.pszWindowTitle = L"CaptureEngine";
        config.pszMainIcon = TD_INFORMATION_ICON;
        config.pszMainInstruction = L"PawnIO was installed successfully";
        config.pszContent =
            L"LibreHardwareMonitor requires administrator privileges to read CPU temperature, package power and core clocks.\n\n"
            L"Would you like to restart CaptureEngine as administrator now?";
        config.cButtons = static_cast<UINT>(std::size(buttons));
        config.pButtons = buttons;
        config.nDefaultButton = kRestartButtonId;
        taskDialog(&config, &choice, nullptr, nullptr);
    } else {
        const int res = MessageBoxW(
            nullptr,
            L"PawnIO was installed successfully.\n\n"
            L"LibreHardwareMonitor requires administrator privileges to read CPU temperature, package power and core clocks.\n\n"
            L"Restart CaptureEngine as administrator now?",
            L"CaptureEngine", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_SETFOREGROUND);
        choice = (res == IDYES) ? kRestartButtonId : kLaterButtonId;
    }

    if (choice == kRestartButtonId) {
        RestartAsAdministrator();
    }
}

int AskWithTaskDialog() {
    HMODULE controls = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!controls)
        return 0;
    auto taskDialog = reinterpret_cast<TaskDialogIndirectFunction>(GetProcAddress(controls, "TaskDialogIndirect"));
    if (!taskDialog)
        return 0;
    const TASKDIALOG_BUTTON buttons[] = {
        {kInstallButtonId, L"Install PawnIO\nRuns the official Microsoft-signed installer bundled with CaptureEngine."},
        {kNotNowButtonId, L"Not now\nAsk again the next time CaptureEngine starts."},
        {kNeverButtonId, L"Don't ask again\nKeep CPU sensors unavailable and stop asking."},
    };
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"CaptureEngine";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Install PawnIO for CPU temperature, power and clock sensors?";
    config.pszContent =
        L"CaptureEngine can show CPU temperature, package power and core clocks in the overlay, but "
        L"LibreHardwareMonitor reads those through PawnIO - a separate, Microsoft-signed kernel driver that is not "
        L"installed on this PC.\n\n"
        L"CaptureEngine bundles the official Microsoft-signed PawnIO installer. Choosing to install runs it "
        L"locally and asks for administrator approval once. GPU sensors work either way.";
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = kNotNowButtonId;
    int pressed = 0;
    if (FAILED(taskDialog(&config, &pressed, nullptr, nullptr)))
        return 0;
    return pressed;
}

int AskWithMessageBox() {
    const int answer = MessageBoxW(
        nullptr,
        L"CaptureEngine can show CPU temperature, package power and core clocks, but LibreHardwareMonitor reads "
        L"those through PawnIO - a separate, Microsoft-signed kernel driver that is not installed on this PC.\n\n"
        L"CaptureEngine bundles the official Microsoft-signed installer for optional offline setup.\n\n"
        L"Yes\tInstall it now (asks for administrator approval once).\n"
        L"No\tNot now; ask again next time.\n"
        L"Cancel\tDon't ask again.\n\n"
        L"GPU sensors work either way.",
        L"CaptureEngine", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    if (answer == IDYES)
        return kInstallButtonId;
    if (answer == IDNO)
        return kNotNowButtonId;
    if (answer == IDCANCEL)
        return kNeverButtonId;
    return 0;
}

void PromptThread() {
    int choice = AskWithTaskDialog();
    if (choice == 0)
        choice = AskWithMessageBox();
    if (choice == kNeverButtonId) {
        SuppressPrompt();
        return;
    }
    if (choice != kInstallButtonId) {
        LogInfo("[PawnIO] Driver installation deferred; CPU sensors stay unavailable this run");
        return;
    }
    const int result = IsProcessElevated() ? InstallDriverNow() : RunElevatedSetupRole(kInstallCommand);
    if (result == 0 && IsDriverInstalled()) {
        LogInfo("[PawnIO] Driver installed; offering restart as administrator");
        OfferRestartAsAdministrator();
        return;
    }
    LogWarn("[PawnIO] Driver installation did not complete (result=%d)", result);
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

void OfferInstallationAsync(const ::HardwareSensorsConfig& config) {
    if (config.enabled == "off" || !AreSensorLibraryFilesPresent())
        return;
    if (config.cpuTemperature == "off" && config.cpuPackagePower == "off" && config.cpuCoreClock == "off")
        return;
    if (IsDriverInstalled() || IsPromptSuppressed())
        return;
    LogInfo("[PawnIO] Driver absent; offering optional installation once");
    std::thread(PromptThread).detach();
}

void InstallDriverAsync() {
    std::thread([]() {
        if (IsDriverInstalled()) {
            MessageBoxW(nullptr, L"The PawnIO driver is already installed on this machine.", L"CaptureEngine",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return;
        }
        const int result = IsProcessElevated() ? InstallDriverNow() : RunElevatedSetupRole(kInstallCommand);
        if (result == 0 && IsDriverInstalled()) {
            ClearPromptSuppression();
            OfferRestartAsAdministrator();
            return;
        }
        MessageBoxW(nullptr, L"PawnIO driver installation was cancelled or did not complete.", L"CaptureEngine",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }).detach();
}

void UninstallDriverAsync() {
    std::thread([]() {
        if (!IsDriverInstalled()) {
            MessageBoxW(nullptr, L"The PawnIO driver is not installed on this machine.", L"CaptureEngine",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return;
        }

        const int confirm = MessageBoxW(
            nullptr,
            L"Uninstall the PawnIO kernel driver?\n\n"
            L"PawnIO is a shared system component. Other hardware monitoring tools on your PC "
            L"(e.g. FanControl, LibreHardwareMonitor) may also rely on it.\n\n"
            L"Are you sure you want to uninstall PawnIO?",
            L"CaptureEngine", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        if (confirm != IDYES)
            return;

        const int result = IsProcessElevated() ? UninstallDriverNow() : RunElevatedSetupRole(kUninstallCommand);
        if (result == 0 && !IsDriverInstalled()) {
            MessageBoxW(nullptr, L"PawnIO driver was successfully uninstalled.", L"CaptureEngine",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return;
        }
        MessageBoxW(nullptr, L"PawnIO driver uninstallation was cancelled or did not complete.", L"CaptureEngine",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }).detach();
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
    if (!IsProcessElevated())
        return RunElevatedSetupRole(install ? kInstallCommand : kUninstallCommand);
    return install ? InstallDriverNow() : UninstallDriverNow();
}

}  // namespace ce::pawnio
