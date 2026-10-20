// clang-format off
#include <windows.h>
// clang-format on
#include "captureengine/roles/role_entries.h"
#include "common/config/config.h"
#include "common/crash/crash_handler.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/role_context.h"
#include "common/logging/logging.h"
#include "common/platform/ansi_path.h"
#include "common/platform/runtime_package_paths.h"

namespace {

// Signalled by the console handler and observed by the role on its next wake. The event lives until the process
// exits, as the handler that signals it does.
HANDLE g_consoleStopEvent = nullptr;

// Console control handler for graceful cleanup. Process-wide, so only the process host installs it (DR-39).
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LogInfo("[Inject] Console event %lu received, shutting down...", ctrlType);
        SetEvent(g_consoleStopEvent);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

// The inject helper process: the process-wide effects of a process that owns itself, then the role over its
// authenticated pipe. The role's own logic lives in RunInjectRole (DR-38).
int InjectProcessMain(const AppConfig& config) {
    InstallCrashHandler();
    Log_SetLevel(config.logLevel);

    g_consoleStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_consoleStopEvent) {
        LogError("[Inject] Failed to create the stop event: %lu", GetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    ProcessIPCServer ipc(ProcessMode::Inject);
    if (!ipc.Init()) {
        LogError("[Inject] Failed to initialize IPC");
        return 1;
    }

    // The package is the executable's directory. Fail closed rather than guess from an unresolvable path: a wrong
    // config.ini silently changes hook behavior, so an unresolvable directory aborts this child instead.
    ce::runtime::PackagePathError pathError = ce::runtime::PackagePathError::None;
    const auto paths = ce::runtime::RuntimePackagePaths::FromExecutable(ce::ansi_path::ModulePathW(nullptr),
                                                                        ce::runtime::ConfigurationArgument{}, pathError);
    if (!paths) {
        LogError("[Inject] Cannot resolve the executable directory reliably (error=%d); refusing ambiguous config path",
                 static_cast<int>(pathError));
        return 1;
    }

    ce::role::RoleContext context;
    context.mode = ProcessMode::Inject;
    context.ownsProcess = true;
    context.processTuning = true;
    context.channel = &ipc;
    context.stopEvent = g_consoleStopEvent;
    context.controllerPid = ipc.ControllerPid();
    context.packageDir = ce::ansi_path::ParentDirectoryW(paths->Executable());
    context.roleHostPath = paths->Executable();
    context.configPath = paths->Configuration();
    return ce::role::RunInjectRole(context, config);
}
