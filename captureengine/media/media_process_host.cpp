// clang-format off
#include <windows.h>
// clang-format on
#include "captureengine/roles/role_entries.h"
#include "common/platform/package_module.h"
#include "common/config/config.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/role_context.h"
#include "common/logging/logging.h"
#include "common/platform/ansi_path.h"
#include "common/platform/runtime_package_paths.h"
#include "media_main_internal.h"

namespace {

// Signalled by the console handler and observed by the role on its loop cadence. The event lives until the process
// exits, as the handler that signals it does.
HANDLE g_consoleStopEvent = nullptr;

// Process-wide, so only the process host installs it (DR-39).
BOOL WINAPI MediaConsoleHandler(DWORD ctrlType) {
    // Handle all console events including Windows shutdown/logoff
    LogInfo("[Media] Console event %lu received, shutting down...", ctrlType);
    SetEvent(g_consoleStopEvent);
    return TRUE;
}

}  // namespace

// The media helper process: the process-wide effects of a process that owns itself, then the role over its
// authenticated pipe. The role's own logic lives in RunMediaRole (DR-38).
int MediaProcessMain(const AppConfig& config) {
    Log_SetLevel(config.logLevel);

    g_consoleStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_consoleStopEvent) {
        LogError("[Media] Failed to create the stop event: %lu", GetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(MediaConsoleHandler, TRUE);

    ProcessIPCServer ipc(ProcessMode::Media);
    if (!ipc.Init()) {
        LogError("[Media] Failed to initialize IPC");
        return 1;
    }

    // The package is the executable's directory. Fail closed rather than guess from an unresolvable path.
    ce::runtime::PackagePathError pathError = ce::runtime::PackagePathError::None;
    const auto paths =
        ce::runtime::RuntimePackagePaths::FromModule(ce::package::PackageModule(),
                                                         ce::runtime::ConfigurationArgument{}, pathError);
    if (!paths) {
        LogError("[Media] Cannot resolve the executable directory reliably (error=%d); refusing ambiguous config path",
                 static_cast<int>(pathError));
        return 1;
    }

    ce::role::RoleContext context;
    context.mode = ProcessMode::Media;
    context.ownsProcess = true;
    context.processTuning = true;
    context.channel = &ipc;
    context.stopEvent = g_consoleStopEvent;
    context.controllerPid = ipc.ControllerPid();
    context.packageDir = ce::ansi_path::ParentDirectoryW(paths->Executable());
    // The executable that runs this helper also runs the helpers it starts: captureengine.exe, or the client's own
    // with CE_FEATURE_SELF_ROLE_HOST (11, part 6).
    context.roleHostPath = ce::ansi_path::ModulePathW(nullptr);
    context.configPath = paths->Configuration();
    context.recordingId = g_RecordingId;
    context.latencyChannelHandle = ParseInheritedLatencyChannelHandle();
    context.resultSlotHandle = ParseInheritedMediaResultHandle();
    return ce::role::RunMediaRole(context, config);
}
