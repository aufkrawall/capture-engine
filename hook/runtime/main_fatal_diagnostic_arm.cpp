#include "main_fatal_diagnostic_arm.h"

#include "common/crash/crash_diagnostic_arm.h"
#include "common/crash/crash_first_chance.h"
#include "hook/runtime/hook_common.h"

namespace ce::diagnostic_arm {

void CheckAndCache(const char* sessionDir, const char* processName) {
    char target[MAX_PATH] = {};
    const bool armed = ce::crash_dump_policy::ReadDiagnosticArmMarker(sessionDir, target, sizeof(target)) &&
                       ce::crash_dump_policy::IsDiagnosticArmMatch(target, processName);
    ce::crash_first_chance::SetDiagnosticArmEnabled(armed);
    if (armed) {
        HookLogImportant("FatalExitDump: Diagnostic arm enabled for %s", processName);
    }
}

void Clear() {
    ce::crash_first_chance::SetDiagnosticArmEnabled(false);
}

}  // namespace ce::diagnostic_arm
