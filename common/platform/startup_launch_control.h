// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "common/platform/raii_helpers.h"
#include <string>

namespace ce::startup_launch {

enum class Object { Role, Ready, Gate, Active, ResumeRequested };
std::wstring ObjectName(Object object, DWORD pid);
bool IsLaunchHost(const std::string& name);
bool CanHostCreationHook(HANDLE process);
bool HasRole(DWORD pid);

class Gate {
public:
    explicit Gate(HANDLE mutex, DWORD timeout = 5000);
    ~Gate();
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    bool Acquired() const { return acquired_; }
    void Release();
private:
    HANDLE mutex_;
    bool acquired_;
};

// A discovered child must wait for its creator's pre-import transaction before
// the ordinary injector starts a remote LoadLibrary thread. No timing heuristic.
bool WaitForCreator(DWORD childPid, DWORD timeout = 5000);

}  // namespace ce::startup_launch
