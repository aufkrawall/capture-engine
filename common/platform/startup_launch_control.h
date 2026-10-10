// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "common/platform/raii_helpers.h"
#include <string>

struct AppConfig;

namespace ce::startup_launch {

enum class Object { Role, Ready, Gate, Active, ResumeRequested, Detach, Detached };
std::wstring ObjectName(Object object, DWORD pid);
bool IsLaunchHost(const std::string& name);
bool IsDesktopShell(const std::string& name);

// [Injection] startup_creator_hosts: which creators may carry the creation-only
// role. Storefronts keeps the launcher chain coverage but never touches the
// desktop shells; off keeps every parent process untouched.
enum class CreatorScope { Auto, Storefronts, Off };
CreatorScope ParseCreatorScope(const std::string& value);
bool HostInCreatorScope(const std::string& name, CreatorScope scope);

// Classification of a child image before its creation is committed. Anything
// that is neither a capture/overlay target nor a launch host must take the
// caller's creation path byte-identically: no gate, no forced suspension and no
// post-creation work, so software that is not an interception target is never
// observable as touched (anti-cheat, overlays and launchers included).
enum class ChildKind { Passthrough, GameTarget, LaunchHost };
ChildKind ClassifyChild(const AppConfig& config, const std::string& name);
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
