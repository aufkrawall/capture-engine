// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The helper side of every CaptureEngine process launch (libcengine 11, part 6): one dispatcher for whatever
// executable runs the helpers. captureengine.exe calls it from WinMain; a client that sets
// CE_FEATURE_SELF_ROLE_HOST calls it through ce_role_host_main() at the start of its own main, so its executable runs
// the role processes, the per-app audio worker, the sensors bridge and the elevated setup actions and no
// captureengine.exe is needed.
//
// Recognized: elevated setup actions (--ce-setup), the per-app audio worker (--process-loopback-worker), the sensors
// bridge (--sensor-bridge), the PawnIO setup role, the crash-dump helper (--dump-helper) and the helper roles
// (--mode=inject|media|logger|sensors). Anything else (no helper command, or --launch) is not a helper launch.

#include <optional>

namespace ce::role_host {

// Runs the helper `commandLine` names (the arguments after the program name, as WinMain receives them) and returns
// its exit code; nullopt when the command line is not a helper launch.
std::optional<int> TryRun(const char* commandLine);

// ce_role_host_main(): the same dispatch in a client's executable, from this process's command line. Only launches
// the runtime makes are served (IsClientHelperLaunch); such a process is the helper's own, so it is made per-monitor
// DPI aware as captureengine.exe's manifest makes it.
std::optional<int> TryRunForClient();

}  // namespace ce::role_host
