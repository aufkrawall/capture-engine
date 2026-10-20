#pragma once

#include "captureengine/diagnostics/logger_ring_source.h"
#include "common/config/config.h"

namespace ce::role {
struct RoleContext;
}

// The logger's process host: the helper process's entry. It reads the command line and the executable's
// directory into a RoleContext, applies the process-only effects (console control, working-set trim) and
// runs ce::role::RunLoggerRole (DR-38, DR-39).
int LoggerProcessMain(const AppConfig& config);

namespace logger_service {
// The logger role over `rings`, which ce::role::RunLoggerRole supplies as the shared named rings. Drains the
// rings into session logs until the context's stop event is set, or, in process mode, the controller's
// shutdown event fires or the controller exits.
int RunLoggerWithRings(ce::role::RoleContext& context, const AppConfig& config, LoggerRingSource& rings);
}  // namespace logger_service
