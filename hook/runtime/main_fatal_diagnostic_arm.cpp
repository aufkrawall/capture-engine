#include "main_fatal_diagnostic_arm.h"

#include <atomic>
#include <mutex>

#include "common/crash/crash_diagnostic_arm.h"
#include "common/crash/crash_first_chance.h"
#include "hook/runtime/hook_common.h"

namespace ce::diagnostic_arm {

namespace {

std::atomic<bool> g_Armed{false};
char g_TargetProcess[MAX_PATH] = {};
std::mutex g_Mutex;

}  // namespace

void CheckAndCache(const char* sessionDir, const char* processName) {
    char target[MAX_PATH] = {};
    const bool found = ce::crash_dump_policy::ReadDiagnosticArmMarker(sessionDir, target, sizeof(target));
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (found && ce::crash_dump_policy::IsDiagnosticArmMatch(target, processName)) {
        g_Armed.store(true, std::memory_order_release);
        std::strncpy(g_TargetProcess, target, sizeof(g_TargetProcess) - 1);
        g_TargetProcess[sizeof(g_TargetProcess) - 1] = '\0';
        ce::crash_first_chance::SetDiagnosticArmEnabled(true);
        HookLogImportant(
            "FatalExitDump: Diagnostic arm ARMED for process=%s (target=%s session=%s) - enables nonzero "
            "self-termination dump and transition trace",
            processName ? processName : "unknown", target, sessionDir ? sessionDir : "unknown");
    } else {
        g_Armed.store(false, std::memory_order_release);
        g_TargetProcess[0] = '\0';
        ce::crash_first_chance::SetDiagnosticArmEnabled(false);
    }
}

bool IsArmed() {
    return g_Armed.load(std::memory_order_acquire);
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Armed.store(false, std::memory_order_release);
    g_TargetProcess[0] = '\0';
    ce::crash_first_chance::SetDiagnosticArmEnabled(false);
}

void SetForTesting(bool armed, const char* targetProcess) {
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Armed.store(armed, std::memory_order_release);
    if (targetProcess) {
        std::strncpy(g_TargetProcess, targetProcess, sizeof(g_TargetProcess) - 1);
        g_TargetProcess[sizeof(g_TargetProcess) - 1] = '\0';
    } else {
        g_TargetProcess[0] = '\0';
    }
    ce::crash_first_chance::SetDiagnosticArmEnabled(armed);
}

const char* GetTargetProcess() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_TargetProcess;
}

}  // namespace ce::diagnostic_arm
