#include "captureengine/diagnostics/logger_service.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "captureengine/roles/role_entries.h"
#include "common/ipc/role_context.h"
#include "common/logging/logging.h"
#include "common/platform/strict_integer_parse.h"

namespace {

// Signalled by the console control handler. Never closed: the handler may run until the process exits.
HANDLE g_consoleStopEvent = nullptr;

// Handle Windows shutdown/logoff when the controller may already be gone: the logger drains its final
// snapshot instead of being terminated.
BOOL WINAPI OnConsoleControl(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        SetEvent(g_consoleStopEvent);
        return TRUE;
    }
    return FALSE;
}

// The controller PID from `--parent-pid=`, or 0 when absent or malformed.
uint32_t ParseControllerPid() {
    uint32_t controllerPid = 0;
    const char* cmdLine = GetCommandLineA();
    const char* pidArg = strstr(cmdLine, "--parent-pid=");
    if (pidArg) {
        const char* value = pidArg + 13;
        const char* end = value;
        while (*end >= '0' && *end <= '9')
            ++end;
        if ((*end == '\0' || *end == ' ' || *end == '\t') &&
            !ce::TryParseUInt32(std::string_view(value, static_cast<size_t>(end - value)), controllerPid)) {
            controllerPid = 0;
        }
    }
    return controllerPid;
}

// `<executable directory>\logs`, created if absent. The executable's directory stands in when that path does
// not fit, and the executable's own path when it has no directory part.
std::string ExecutableLogsDirectory() {
    char directory[MAX_PATH]{};
    GetModuleFileNameA(NULL, directory, MAX_PATH);
    char* lastSlash = strrchr(directory, '\\');
    if (!lastSlash)
        return directory;
    *lastSlash = '\0';
    char logsDirectory[MAX_PATH];
    const int written = snprintf(logsDirectory, sizeof(logsDirectory), "%s\\logs", directory);
    if (written <= 0 || written >= static_cast<int>(sizeof(logsDirectory)))
        return directory;
    CreateDirectoryA(logsDirectory, NULL);
    return logsDirectory;
}

}  // namespace

int LoggerProcessMain(const AppConfig& config) {
    // Logging off: the logger does not start, so it installs no process-wide effect either.
    if (!IsAnyLoggingEnabled(config.logLevel)) {
        return 0;
    }
    g_consoleStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_consoleStopEvent) {
        LogError("[Logger] Cannot create the console stop event (error=%lu)", GetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    ce::role::RoleContext context;
    context.mode = ProcessMode::Logger;
    context.ownsProcess = true;
    context.controllerPid = ParseControllerPid();
    context.stopEvent = g_consoleStopEvent;
    context.logsRoot = ExecutableLogsDirectory();
    return ce::role::RunLoggerRole(context, config);
}
