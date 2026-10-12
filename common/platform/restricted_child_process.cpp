#include "restricted_child_process.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace ce::process {

RestrictedChildProcess::~RestrictedChildProcess() {
    if (processHandle) {
        CloseHandle(processHandle);
    }
}

RestrictedChildProcess::RestrictedChildProcess(RestrictedChildProcess&& other) noexcept {
    *this = std::move(other);
}

RestrictedChildProcess& RestrictedChildProcess::operator=(RestrictedChildProcess&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (processHandle) {
        CloseHandle(processHandle);
    }
    processHandle = std::exchange(other.processHandle, nullptr);
    processId = std::exchange(other.processId, 0);
    return *this;
}

bool LaunchRestrictedChildProcess(const std::wstring& executablePath, const std::wstring& commandLine,
                                  const std::wstring& workingDirectory, const std::vector<HANDLE>& inheritedHandles,
                                  DWORD creationFlags, RestrictedChildProcess& child, DWORD& error,
                                  HANDLE ownershipJob) {
    error = ERROR_SUCCESS;
    child = {};
    if (executablePath.empty() || commandLine.empty()) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    for (HANDLE handle : inheritedHandles) {
        DWORD flags = 0;
        if (!handle || handle == INVALID_HANDLE_VALUE || !GetHandleInformation(handle, &flags) ||
            (flags & HANDLE_FLAG_INHERIT) == 0) {
            error = ERROR_INVALID_HANDLE;
            return false;
        }
    }

    const DWORD attributeCount = (inheritedHandles.empty() ? 0u : 1u) + (ownershipJob ? 1u : 0u);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = attributeCount == 0 ? sizeof(STARTUPINFOW) : static_cast<DWORD>(sizeof(startup));
    // CaptureEngine is a GUI-subsystem image even for its windowless internal
    // roles. Without this opt-out Windows applies the process-start feedback
    // cursor to every media/logger/sensor/limiter/app-audio worker spawn.
    startup.StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK;
    std::vector<uint8_t> attributeStorage;
    if (attributeCount != 0) {
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &attributeBytes);
        if (attributeBytes == 0) {
            error = GetLastError();
            return false;
        }
        attributeStorage.resize(attributeBytes);
        startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, attributeCount, 0, &attributeBytes)) {
            error = GetLastError();
            return false;
        }
        if (!inheritedHandles.empty() &&
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       reinterpret_cast<void*>(const_cast<HANDLE*>(inheritedHandles.data())),
                                       inheritedHandles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            error = GetLastError();
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            return false;
        }
        // Job membership is installed by CreateProcess before the child's first instruction.
        // The parent's kill-on-close handle is deliberately absent from the inheritance list.
        if (ownershipJob && !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                                      static_cast<void*>(&ownershipJob), sizeof(ownershipJob),
                                                      nullptr, nullptr)) {
            error = GetLastError();
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            return false;
        }
    }

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executablePath.c_str(), mutableCommandLine.data(), nullptr, nullptr, inheritedHandles.empty() ? FALSE : TRUE,
        creationFlags | (attributeCount == 0 ? 0 : EXTENDED_STARTUPINFO_PRESENT), nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup.StartupInfo, &process);
    error = created ? ERROR_SUCCESS : GetLastError();
    if (startup.lpAttributeList)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
        return false;
    }
    CloseHandle(process.hThread);
    child.processHandle = process.hProcess;
    child.processId = process.dwProcessId;
    return true;
}

}  // namespace ce::process
