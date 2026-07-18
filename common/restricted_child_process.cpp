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
                                  DWORD creationFlags, RestrictedChildProcess& child, DWORD& error) {
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

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = inheritedHandles.empty() ? sizeof(STARTUPINFOW) : static_cast<DWORD>(sizeof(startup));
    // CaptureEngine is a GUI-subsystem image even for its windowless internal
    // roles. Without this opt-out Windows applies the process-start feedback
    // cursor to every media/logger/sensor/limiter/app-audio worker spawn.
    startup.StartupInfo.dwFlags = STARTF_FORCEOFFFEEDBACK;
    std::vector<uint8_t> attributeStorage;
    if (!inheritedHandles.empty()) {
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (attributeBytes == 0) {
            error = GetLastError();
            return false;
        }
        attributeStorage.resize(attributeBytes);
        startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
            error = GetLastError();
            return false;
        }
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       const_cast<HANDLE*>(inheritedHandles.data()),
                                       inheritedHandles.size() * sizeof(HANDLE), nullptr, nullptr)) {
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
        creationFlags | (inheritedHandles.empty() ? 0 : EXTENDED_STARTUPINFO_PRESENT), nullptr,
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
