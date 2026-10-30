#include "injection_internal.h"

#include <tlhelp32.h>

#include "common/config/config.h"
#include "common/logging/logging.h"
#include "common/platform/process_parent.h"
#include "common/platform/utf8.h"
#include "common/graphics/ngx_ota_policy.h"

namespace {

struct UpdaterParent {
    DWORD pid = 0;
    std::string name;  // UTF-8 base name; empty when the parent is gone or reused
};

// The process that launched `updater`, identified only when the process now
// holding that PID existed before the updater did.
UpdaterParent QueryUpdaterParent(HANDLE updater) {
    UpdaterParent parent;
    if (!ce::process_parent::QueryParentProcessId(updater, &parent.pid) || parent.pid == 0)
        return parent;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent.pid);
    if (!process)
        return parent;
    if (ce::process_parent::ParentPredatesChild(ce::process_parent::CreationTime(process),
                                                ce::process_parent::CreationTime(updater))) {
        const std::wstring name = ce::process_parent::ImageBaseName(process);
        if (!name.empty())
            parent.name = ce::process_start::ExtractFileName(name.c_str());
    }
    CloseHandle(process);
    return parent;
}

}  // namespace

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
    const char* tag = sourceTag ? sourceTag : "Watchdog";
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        // Only an updater one of CE's own targets launched is CE's to stop.
        const UpdaterParent parent = QueryUpdaterParent(hProcess);
        const bool parentIsTarget = !parent.name.empty() && IsWhitelisted(parent.pid, parent.name);
        if (!ce::ngx_ota::ShouldHostTerminateUpdater(kNgxOtaModeOff, parentIsTarget)) {
            CloseHandle(hProcess);
            LogInfo("[%s] Left NGX updater %s (PID: %lu) running: parent %s (PID: %lu) is not a CaptureEngine target",
                    tag, imageName.c_str(), static_cast<unsigned long>(pid),
                    parent.name.empty() ? "<exited or unreadable>" : parent.name.c_str(),
                    static_cast<unsigned long>(parent.pid));
            return true;
        }
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        LogInfo("[%s] Terminated NGX updater %s (PID: %lu) launched by target %s (PID: %lu) because ngx_ota=off", tag,
                imageName.c_str(), static_cast<unsigned long>(pid), parent.name.c_str(),
                static_cast<unsigned long>(parent.pid));
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_INVALID_PARAMETER) {
        LogDebug("[%s] NGX updater %s (PID: %lu) already exited", tag, imageName.c_str(),
                 static_cast<unsigned long>(pid));
    } else {
        LogWarn("[%s] Failed to open spawned NGX updater %s (PID: %lu): error=%lu", tag, imageName.c_str(),
                static_cast<unsigned long>(pid), err);
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
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            const std::string imageName = ce::utf8::FromWide(pe32.szExeFile);
            if (ce::ngx_ota::IsNgxUpdaterImage(imageName.c_str())) {
                TerminateNgxUpdaterIfDisabled(pe32.th32ProcessID, imageName, reason);
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}
