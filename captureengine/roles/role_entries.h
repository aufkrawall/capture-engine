#pragma once

#include "common/ipc/role_context.h"

struct AppConfig;

// The role entries both hosts call (DR-36, DR-38): the helper process's WinMain through
// `<Role>ProcessMain`, and the runtime's thread host. Each returns the role's exit code (media: the
// coded finalization, common/ipc/media_finalization.h). The context and the configuration outlive the call.
namespace ce::role {
int RunInjectRole(RoleContext& context, const AppConfig& config);
int RunMediaRole(RoleContext& context, const AppConfig& config);
int RunLoggerRole(RoleContext& context, const AppConfig& config);
}  // namespace ce::role
