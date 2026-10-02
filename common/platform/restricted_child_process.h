#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ce::process {

struct RestrictedChildProcess {
    HANDLE processHandle = nullptr;
    DWORD processId = 0;

    RestrictedChildProcess() = default;
    ~RestrictedChildProcess();
    RestrictedChildProcess(const RestrictedChildProcess&) = delete;
    RestrictedChildProcess& operator=(const RestrictedChildProcess&) = delete;
    RestrictedChildProcess(RestrictedChildProcess&& other) noexcept;
    RestrictedChildProcess& operator=(RestrictedChildProcess&& other) noexcept;
};

bool LaunchRestrictedChildProcess(const std::wstring& executablePath, const std::wstring& commandLine,
                                  const std::wstring& workingDirectory, const std::vector<HANDLE>& inheritedHandles,
                                  DWORD creationFlags, RestrictedChildProcess& child, DWORD& error);

}  // namespace ce::process
