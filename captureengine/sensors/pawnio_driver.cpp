#include "pawnio_driver.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "common/logging/logging.h"
#include "pawnio_setup.h"

namespace ce::pawnio {
namespace {

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

std::vector<std::wstring> ReadRegistryMultiString(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    DWORD type = 0;
    DWORD bytes = 0;
    std::vector<std::wstring> results;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && bytes > sizeof(wchar_t)) {
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, valueName, nullptr, nullptr, reinterpret_cast<BYTE*>(buffer.data()), &bytes) ==
            ERROR_SUCCESS) {
            const wchar_t* p = buffer.data();
            while (*p) {
                results.emplace_back(p);
                p += wcslen(p) + 1;
            }
        }
    }
    RegCloseKey(key);
    return results;
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
    else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

std::wstring PnputilPath() {
    wchar_t sysDir[MAX_PATH];
    const UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(sysDir) + L"\\pnputil.exe";
    }
    return L"C:\\Windows\\System32\\pnputil.exe";
}

std::vector<std::wstring> FindPawnIoOemInfs() {
    std::set<std::wstring> uniqueInfs;

    // 1. Check Owners in driver service registry key
    std::vector<std::wstring> owners = ReadRegistryMultiString(HKEY_LOCAL_MACHINE, kDriverServiceKey, L"Owners");
    for (const std::wstring& owner : owners) {
        if (!owner.empty() && owner.find(L".inf") != std::wstring::npos) {
            uniqueInfs.insert(owner);
        }
    }

    // 2. Scan C:\Windows\INF\oem*.inf for pawnio mentions
    WIN32_FIND_DATAW findData = {};
    HANDLE hFind = FindFirstFileW(L"C:\\Windows\\INF\\oem*.inf", &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring infPath = L"C:\\Windows\\INF\\" + std::wstring(findData.cFileName);
                HANDLE file = CreateFileW(infPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    char buf[4096];
                    DWORD read = 0;
                    if (ReadFile(file, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
                        buf[read] = '\0';
                        for (DWORD i = 0; i < read; ++i)
                            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(buf[i])));
                        if (strstr(buf, "pawnio") != nullptr) {
                            uniqueInfs.insert(findData.cFileName);
                        }
                    }
                    CloseHandle(file);
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    return {uniqueInfs.begin(), uniqueInfs.end()};
}

void RemovePawnIoDriverPackages() {
    const std::vector<std::wstring> infs = FindPawnIoOemInfs();
    const std::wstring pnputil = PnputilPath();
    for (const std::wstring& inf : infs) {
        std::wstring commandLine;
        commandLine.reserve(pnputil.size() + inf.size() + 40);
        commandLine += L"\"";
        commandLine += pnputil;
        commandLine += L"\" /delete-driver ";
        commandLine += inf;
        commandLine += L" /uninstall /force";
        LogInfo("[PawnIO] Removing driver package via pnputil: %ls", commandLine.c_str());
        const int res = RunProcessAndWait(pnputil, commandLine);
        LogInfo("[PawnIO] pnputil for %ls returned %d", inf.c_str(), res);
    }
}

bool StopAndDeleteDriverService(const wchar_t* serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        LogWarn("[PawnIO] Cannot open Service Control Manager (error=%lu)", GetLastError());
        return false;
    }

    SC_HANDLE service = OpenServiceW(scm, serviceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            LogInfo("[PawnIO] Service %ls does not exist", serviceName);
            return true;
        }
        LogWarn("[PawnIO] Cannot open service %ls (error=%lu)", serviceName, err);
        return false;
    }

    SERVICE_STATUS status = {};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        LogInfo("[PawnIO] Sent STOP control to %ls service", serviceName);
        for (int i = 0; i < 30; ++i) {
            if (QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_STOPPED) {
                LogInfo("[PawnIO] Service %ls stopped successfully", serviceName);
                break;
            }
            Sleep(100);
        }
    }

    bool deleted = DeleteService(service) != FALSE;
    if (!deleted) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_MARKED_FOR_DELETE) {
            LogInfo("[PawnIO] Service %ls was already marked for deletion", serviceName);
            deleted = true;
        } else {
            LogWarn("[PawnIO] DeleteService failed for %ls (error=%lu)", serviceName, err);
        }
    } else {
        LogInfo("[PawnIO] DeleteService succeeded for %ls", serviceName);
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return deleted;
}

void CleanLeftoverPawnIoKeysAndFiles() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kDriverServiceKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO");

    const std::filesystem::path installDir = L"C:\\Program Files\\PawnIO";
    std::error_code ec;
    if (std::filesystem::exists(installDir, ec)) {
        std::filesystem::remove_all(installDir, ec);
        LogInfo("[PawnIO] Cleaned %ls (ec=%d)", installDir.c_str(), ec.value());
    }
}

}  // namespace

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
    LogInfo("[PawnIO] Starting complete driver uninstallation...");

    // 1. Run registered uninstaller if present
    std::wstring command = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"QuietUninstallString");
    if (command.empty())
        command = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString");
    if (!command.empty()) {
        LogInfo("[PawnIO] Running registered uninstall command: %ls", command.c_str());
        RunProcessAndWait(std::wstring(), command);
    }

    // 2. Run bundled uninstaller if present
    const std::filesystem::path setupPath = BundledPawnIoSetupPath();
    if (VerifyPawnIoSetupBinary(setupPath)) {
        std::wstring commandLine = L"\"" + setupPath.wstring() + L"\" -uninstall -silent";
        LogInfo("[PawnIO] Running uninstallation via bundled installer: %ls", setupPath.c_str());
        RunProcessAndWait(setupPath.wstring(), commandLine);
    }

    // 3. Stop and delete the kernel driver service via Service Control Manager
    StopAndDeleteDriverService(L"PawnIO");

    // 4. Remove all PawnIO driver packages from DriverStore via pnputil
    RemovePawnIoDriverPackages();

    // 5. Clean up any leftover registry keys and files
    CleanLeftoverPawnIoKeysAndFiles();

    const bool stillInstalled = IsDriverInstalled();
    LogInfo("[PawnIO] Driver uninstallation finished; still installed = %d", stillInstalled ? 1 : 0);
    return stillInstalled ? 1 : 0;
}

}  // namespace ce::pawnio
