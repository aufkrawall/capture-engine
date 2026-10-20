#pragma once

// What the media role derives from its RoleContext (DR-38) without process state: the settings and cache paths, and
// the wait between its loop polls. Kept apart from the session so tests drive it directly.

#include <windows.h>

#include <atomic>
#include <string>

namespace ce::media_role {

struct RolePaths {
    // The settings file the role reads (active code page): the context's argument, else config.ini in the package.
    std::string configPath;
    // The directory of configPath; the per-device render-latency cache lives there.
    std::string mediaCacheDir;
};

// `packageDir` is the package directory without a trailing separator; `configPathArgument` is RoleContext::configPath
// and may be empty.
RolePaths DeriveRolePaths(const std::wstring& packageDir, const std::string& configPathArgument);

// Waits up to `ms` between two polls of the role loop. Returns true when `stop` is signaled, which ends the loop. A
// null or unusable handle sleeps instead, so the loop keeps its cadence.
bool WaitForPollOrStop(HANDLE stop, DWORD ms);

// Whether the settings' process priority class and GPU scheduling priority may be applied to the session's process
// (DR-39): only with CE_FEATURE_PROCESS_TUNING, which the context carries as `processTuning`. A refusal is logged once
// per session; `loggedRefusal` records that it was.
bool AllowsProcessTuning(bool processTuning, std::atomic<bool>& loggedRefusal);

// Closes the handles a RoleContext hands the media role to own (result slot, latency channel) and clears them. For the
// paths that refuse to run before the session maps them; a running session maps and releases them itself.
void ReleaseRoleOwnedHandles(void*& resultSlotHandle, void*& latencyChannelHandle);

}  // namespace ce::media_role
