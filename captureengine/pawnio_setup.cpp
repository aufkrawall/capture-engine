#include "pawnio_setup.h"

#include <commctrl.h>
#include <shellapi.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "../common/config.h"
#include "../common/logging.h"
#include "sensor_selection_policy.h"

namespace ce::pawnio {
namespace {

constexpr wchar_t kDriverServiceKey[] = L"SYSTEM\\CurrentControlSet\\Services\\PawnIO";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";
constexpr wchar_t kSuppressionKey[] = L"Software\\CaptureEngine";
constexpr wchar_t kSuppressionValue[] = L"PawnIoPromptSuppressed";
constexpr wchar_t kPackageIdentifier[] = L"namazso.PawnIO";
constexpr wchar_t kProjectPage[] = L"https://pawnio.eu/";

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

// Locates winget without trusting PATH alone: the per-user app-execution alias
// is the canonical location and stays valid under same-user elevation.
std::filesystem::path FindPackageManager() {
    wchar_t localAppData[MAX_PATH] = {};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, static_cast<DWORD>(std::size(localAppData)));
    if (length > 0 && length < std::size(localAppData)) {
        const std::filesystem::path alias =
            std::filesystem::path(localAppData) / L"Microsoft" / L"WindowsApps" / L"winget.exe";
        if (GetFileAttributesW(alias.c_str()) != INVALID_FILE_ATTRIBUTES)
            return alias;
    }
    wchar_t resolved[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"winget.exe", nullptr, static_cast<DWORD>(std::size(resolved)), resolved, nullptr) > 0)
        return resolved;
    return {};
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
    // The package manager downloads and runs a vendor installer; several minutes
    // is generous but bounded, so a wedged child cannot keep the elevated role
    // alive forever.
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

int InstallDriverNow() {
    const std::filesystem::path packageManager = FindPackageManager();
    if (packageManager.empty()) {
        // Without a package source there is nothing CaptureEngine can safely
        // execute: it will not download or run a kernel-driver installer itself.
        LogWarn("[PawnIO] Windows Package Manager is unavailable; opening the project page instead");
        ShellExecuteW(nullptr, L"open", kProjectPage, nullptr, nullptr, SW_SHOWNORMAL);
        return 2;
    }
    std::wstring commandLine = L"\"" + packageManager.wstring() +
                               L"\" install --id " + kPackageIdentifier +
                               L" --exact --source winget --accept-source-agreements --accept-package-agreements"
                               L" --disable-interactivity";
    LogInfo("[PawnIO] Installing the driver through the Windows Package Manager");
    const int result = RunProcessAndWait(packageManager.wstring(), commandLine);
    LogInfo("[PawnIO] Package manager finished with exit code %d", result);
    return result == 0 ? 0 : 1;
}

int UninstallDriverNow() {
    std::wstring command = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"QuietUninstallString");
    if (command.empty())
        command = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString");
    if (command.empty()) {
        LogWarn("[PawnIO] No registered uninstall command; nothing to do");
        return 2;
    }
    LogInfo("[PawnIO] Running the driver's own registered uninstall command");
    return RunProcessAndWait(std::wstring(), command) == 0 ? 0 : 1;
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

// Three genuinely distinct answers need three buttons, which MessageBox cannot
// express. TaskDialog is resolved at runtime so an environment without the v6
// common controls degrades instead of failing.
int AskWithTaskDialog() {
    HMODULE controls = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!controls)
        return 0;
    auto taskDialog = reinterpret_cast<TaskDialogIndirectFunction>(GetProcAddress(controls, "TaskDialogIndirect"));
    if (!taskDialog)
        return 0;
    const TASKDIALOG_BUTTON buttons[] = {
        {kInstallButtonId, L"Install PawnIO\nRuns the official installer through the Windows Package Manager."},
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
        L"CaptureEngine does not bundle or download the driver. Choosing to install hands the job to the Windows "
        L"Package Manager and asks for administrator approval once. GPU sensors work either way.";
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
        L"Yes\tInstall it now through the Windows Package Manager (asks for administrator approval).\n"
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
        LogInfo("[PawnIO] Driver installed; restart CaptureEngine as administrator to read the CPU rails");
        MessageBoxW(nullptr,
                    L"PawnIO was installed.\n\nRestart CaptureEngine as administrator to read CPU temperature, "
                    L"package power and core clocks.",
                    L"CaptureEngine", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return;
    }
    LogWarn("[PawnIO] Driver installation did not complete (result=%d)", result);
}

}  // namespace

bool IsDriverInstalled() {
    // The driver package registers a service key; on 64-bit Windows the caller
    // must not be redirected into the WOW6432Node view.
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

void OfferInstallationAsync(const ::HardwareSensorsConfig& config) {
    if (config.enabled == "off" || !AreSensorLibraryFilesPresent())
        return;
    // Only the rails that actually need the driver justify a prompt.
    if (config.cpuTemperature == "off" && config.cpuPackagePower == "off" && config.cpuCoreClock == "off")
        return;
    if (IsDriverInstalled() || IsPromptSuppressed())
        return;
    LogInfo("[PawnIO] Driver absent; offering the optional installation once");
    std::thread(PromptThread).detach();
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
        return ERROR_ELEVATION_REQUIRED;
    return install ? InstallDriverNow() : UninstallDriverNow();
}

}  // namespace ce::pawnio
