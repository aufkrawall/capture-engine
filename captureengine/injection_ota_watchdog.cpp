#include "injection_internal.h"

#include <tlhelp32.h>

#include "../common/config.h"
#include "../common/logging.h"
#include "../hook/common/ngx_ota_policy.h"

void InjectionManager::SetNgxOtaModeQuery(NgxOtaModeQuery query) {
    std::lock_guard<std::mutex> lock(configMutex);
    ngxOtaModeQuery = std::move(query);
}

bool InjectionManager::IsNgxOtaDisabled() {
    NgxOtaModeQuery query;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        query = ngxOtaModeQuery;
    }
    if (query) {
        const uint8_t queryMode = query();
        if (queryMode == kNgxOtaModeOff) {
            return true;
        }
        if (queryMode == kNgxOtaModeOn) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(configMutex);
    return ParseNgxOtaMode(config.graphics.ngxOta) == kNgxOtaModeOff;
}

bool InjectionManager::TerminateNgxUpdaterIfDisabled(DWORD pid, const std::string& imageName, const char* sourceTag) {
    if (!ce::ngx_ota::IsNgxUpdaterImage(imageName.c_str())) {
        return false;
    }
    if (!IsNgxOtaDisabled()) {
        return false;
    }
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        LogInfo("[%s] Terminated spawned NGX updater %s (PID: %lu) because ngx_ota=off",
                sourceTag ? sourceTag : "Watchdog", imageName.c_str(), static_cast<unsigned long>(pid));
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_INVALID_PARAMETER) {
        LogDebug("[%s] NGX updater %s (PID: %lu) already exited",
                 sourceTag ? sourceTag : "Watchdog", imageName.c_str(), static_cast<unsigned long>(pid));
    } else {
        LogWarn("[%s] Failed to terminate spawned NGX updater %s (PID: %lu): error=%lu",
                sourceTag ? sourceTag : "Watchdog", imageName.c_str(), static_cast<unsigned long>(pid), err);
    }
    return true;
}

void InjectionManager::SweepRunningNgxUpdatersIfDisabled(const char* reason) {
    if (!IsNgxOtaDisabled()) {
        return;
    }
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (ce::ngx_ota::IsNgxUpdaterImage(pe32.szExeFile)) {
                TerminateNgxUpdaterIfDisabled(pe32.th32ProcessID, pe32.szExeFile, reason);
            }
        } while (Process32Next(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}
