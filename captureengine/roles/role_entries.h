#pragma once

#include "common/ipc/role_context.h"

#include <cstdint>
#include <string>
#include <vector>

struct AppConfig;

// The role entries both hosts call (DR-36, DR-38): the helper process's WinMain through
// `<Role>ProcessMain`, and the runtime's thread host. Each returns the role's exit code (media: the
// coded finalization, common/ipc/media_finalization.h). The context and the configuration outlive the call.
namespace ce::role {
int RunInjectRole(RoleContext& context, const AppConfig& config);
int RunMediaRole(RoleContext& context, const AppConfig& config);
int RunLoggerRole(RoleContext& context, const AppConfig& config);

// A recording the recovery repaired (libcengine 11, part 3).
struct RecoveredRecording {
    std::wstring output;  // the repaired file
    int64_t endUs = 0;    // where its tracks end
};

// Repairs the interrupted recordings this Windows user's in-flight journal names (common/capture/inflight_journal.h)
// whose writer is gone. Loads mediaengine.dll from `packageDir`, scoped to this load, only when the journal has
// entries. `cancel` (nonzero) stops between and inside files; what is left stays in the journal for the next run.
// Runs on the caller's thread and may take as long as copying the files does.
std::vector<RecoveredRecording> RecoverInterruptedRecordings(const std::wstring& packageDir,
                                                             const volatile long* cancel);

// The helper side of a launch in a client's own executable (CE_FEATURE_SELF_ROLE_HOST, libcengine 11 part 6): runs the
// helper this process's command line names, as captureengine.exe's WinMain would, and returns its exit code; -1 when
// this process is not a helper launch the runtime made (captureengine/app/role_host.h).
int RunClientHelperLaunch();
}  // namespace ce::role
