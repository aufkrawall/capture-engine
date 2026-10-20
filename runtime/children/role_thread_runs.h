// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Helper roles that run on runtime threads (DR-37, DR-38): one run per spawned thread. A run owns what its role takes
// from the runtime (the configuration copy, the role context, the role end of the channel, the stop event and the
// thread handle) until the thread's handle is released after the thread has exited. The thread handle is the lifecycle
// token, so waits, retirement and the media finalization bookkeeping stay the process path's.

#include "runtime/children/child_process_lifecycle.h"
#include "runtime/children/host_children.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace ce::runtime::detail {
class RoleThreadRuns {
public:
    RoleThreadRuns(std::string configPath, ThreadRoleSetup setup);
    ~RoleThreadRuns();
    RoleThreadRuns(const RoleThreadRuns&) = delete;
    RoleThreadRuns& operator=(const RoleThreadRuns&) = delete;

    struct Spawned {
        ChildToken token = 0;
        std::unique_ptr<ce::role::HelperClient> host;  // the supervisor's end of the role's channel; null without one
    };
    // Starts the role's thread. `resultSlot` is the spawner's handle of a media role's result slot; the role gets a
    // duplicate, as it would an inherited handle. Empty on failure: the role did not start.
    Spawned Spawn(ChildRole role, void* resultSlot);
    bool Owns(ChildToken token) const;
    // Sets the stop event of a thread run; a no-op for any other token (a process).
    void RequestStop(ChildToken token) const;
    // The exit code of a run's thread; STILL_ACTIVE when it cannot be read.
    uint32_t ExitCode(ChildToken token) const;
    // Releases a run whose thread has exited: its handle, context, configuration and channel end. A run whose thread
    // still runs is kept.
    void Release(ChildToken token);

private:
    struct Run;
    // The thread entry: runs the role with its run's context and configuration, returns its exit code.
    static DWORD WINAPI RunRoleThread(LPVOID parameter);
    Run* Find(ChildToken token) const;

    std::string configPath_;
    ThreadRoleSetup setup_;
    std::vector<std::unique_ptr<Run>> runs_;
};
}  // namespace ce::runtime::detail
