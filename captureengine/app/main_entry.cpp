#include "main_internal.h"

#include "common/platform/path_utils.h"
#include "captureengine/elevation/startup_control.h"

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (const auto setupResult = ce::startup::TryRunSetup()) return *setupResult;
    if (const std::optional<int> workerResult = TryRunProcessLoopbackWorkerHost()) {
        return *workerResult;
    }

    // The optional hardware-sensor bridge hosts the .NET runtime and the
    // LibreHardwareMonitor library. It runs before config, logging and crash
    // handling so that role stays contained to its own short-lived process.
    if (const std::optional<int> bridgeResult = ce::hardware_sensors::TryRunSensorBridgeHost()) {
        return *bridgeResult;
    }

    // Elevated PawnIO setup role. It runs before every other role for the same
    // reason as the bridge: the action is self-contained and must not depend on
    // controller state.
    if (const std::optional<int> setupResult = ce::pawnio::TryRunPawnIoSetupHost()) {
        return *setupResult;
    }

    if (IsDumpHelperCommandLine(lpCmdLine)) {
        return RunDumpHelperFromCommandLine();
    }

    if (HasExactCommandLineArgument(L"--ce-service-integration"))
        return static_cast<int>(ce::startup::RunElevationIntegration());
    if (HasExactCommandLineArgument(L"--ce-service-fixture"))
        return static_cast<int>(ce::startup::RunElevationFixture(HasExactCommandLineArgument(L"--ce-fixture-wait")));

    // Parse process mode from command line
    ProcessMode mode = ParseProcessMode(lpCmdLine);
    if (const auto startupResult = ce::startup::Bootstrap(mode == ProcessMode::Controller &&
        !HasExactCommandLineArgument(L"--list-monitors") && !HasExactCommandLineArgument(L"--license")))
        return *startupResult;
    if (mode == ProcessMode::Controller && HasExactCommandLineArgument(L"--list-monitors")) {
        return ce::monitor_selection::WriteMonitorListToStandardOutput();
    }
    if (mode == ProcessMode::Controller) {
        // An external launcher may have requested process-start feedback. Clear
        // it before config, logging, registration, or child startup; internal
        // roles must never change the user's current cursor themselves.
        PrimeStartupCursor();
    }

    // Get paths. Config, logs and crash handling open files through the ANSI
    // APIs; an installation folder the code page cannot express would reach them
    // '?'-mangled (config silently at defaults, no logs), so it is resolved from
    // the Unicode module path into an exact ANSI or 8.3 form.
    ce::runtime::PackagePathError pathError = ce::runtime::PackagePathError::None;
    const auto paths = ce::runtime::RuntimePackagePaths::FromModule(
        nullptr, ce::runtime::ReadProcessConfigurationArgument(), pathError);
    if (!paths) {
        OutputDebugStringA("[CaptureEngine] Invalid runtime executable/configuration paths\n");
        return 1;
    }
    const bool runtimePathsExact = paths->EncodingExact();
    const std::string& baseDir = paths->Directory();
    ControllerLaunch launch;
    launch.configPath = paths->Configuration();

    // Load config early so directory and crash-handler setup can be gated on
    // the configured log_level. When log_level=none/off we skip everything to
    // guarantee the logs/ tree stays absent and no debug machinery runs.
    ce::runtime::RuntimeConfigurationSession configuration(launch.configPath);
    if (!configuration.IsReady()) {
        OutputDebugStringA("[CaptureEngine] Cannot acquire runtime configuration ownership\n");
        return 1;
    }

    std::string logsRootDir = baseDir + "\\logs";
    // Session symbol archives are hard links into one shared store below the
    // logs root instead of a ~180 MB copy per session (see crash_symbol_store.h).
    SetCrashSymbolStoreRoot(logsRootDir);

    // Determine session directory: Controller generates a new timestamped folder,
    // child processes inherit the name from --session-dir= on the command line.
    if (mode == ProcessMode::Controller) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[32];
        snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        g_SessionDirName = ts;
        if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
            CleanupOldSessionDirs(logsRootDir);
        }
    } else {
        g_SessionDirName = ParseSessionDir(lpCmdLine);
        g_RecordingId = ParseRecordingId(lpCmdLine);
    }

    std::string earlyLogsDir;
    if (!g_SessionDirName.empty()) {
        earlyLogsDir = logsRootDir + "\\" + g_SessionDirName;
    } else {
        earlyLogsDir = logsRootDir;
    }

    if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
        CreateDirectoryA(logsRootDir.c_str(), NULL);
        CreateDirectoryA(earlyLogsDir.c_str(), NULL);
        // Without a session directory name yet, `earlyLogsDir` IS the logs root — a fallback
        // for an early dump, not a session artifact directory. Archiving the installed symbols
        // there leaves a stray `logs\symbols` full of PDBs next to the per-session folders.
        SetCrashDumpDirectory(earlyLogsDir, /*archiveInstalledSymbols=*/!g_SessionDirName.empty());
        InstallCrashHandler();
        // Earlier builds wrote WER LocalDumps values under HKCU as a "last
        // resort" for fail-fast terminations. WER reads LocalDumps from HKLM
        // only, so they never produced a dump and only accumulated one stale
        // subkey per game, each carrying this user's own session paths.
        ce::wer_dump_adoption::PurgeInertCaptureEngineLocalDumpsRegistration(logsRootDir);
    } else {
        OutputDebugStringA(
            "[CaptureEngine] log_level=none: skipping log directory creation, crash handler, and all debug "
            "machinery\n");
    }

    // Parse --auto-record flag: --auto-record=DELAY_MS,DURATION_MS
    // Parse --license flag
    std::string cmdLine(lpCmdLine);
    if (cmdLine.find("--license") != std::string::npos) {
        std::string licensePath = baseDir + "\\LICENSE.txt";
        ShellExecuteA(NULL, "open", licensePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    size_t autoRecordPos = cmdLine.find("--auto-record=");
    if (autoRecordPos != std::string::npos) {
        std::string params = cmdLine.substr(autoRecordPos + 14);
        const size_t tokenEnd = params.find_first_of(" \t");
        if (tokenEnd != std::string::npos)
            params.resize(tokenEnd);
        size_t commaPos = params.find(',');
        if (commaPos != std::string::npos) {
            DWORD delayMs = 0;
            DWORD durationMs = 0;
            if (TryParseAutoRecordValue(std::string_view(params).substr(0, commaPos), delayMs) &&
                TryParseAutoRecordValue(std::string_view(params).substr(commaPos + 1), durationMs)) {
                launch.autoRecordDelayMs = delayMs;
                launch.autoRecordDurationMs = durationMs;
                launch.autoRecord = true;
            } else {
                LogWarn("[Controller] Ignoring malformed --auto-record value '%s'", params.c_str());
            }
        }
    }

    // Parse --launch flag: --launch <command> or --launch=<command>
    // Supports Steam Launch Options: "CaptureEngine.exe" --launch %command%
    std::string searchFlag = "--launch";
    size_t launchPos = cmdLine.find(searchFlag);
    if (launchPos != std::string::npos) {
        size_t valueStart = launchPos + searchFlag.length();

        // Skip delimiter (+, =, or space)
        while (valueStart < cmdLine.length() && (cmdLine[valueStart] == '=' || cmdLine[valueStart] == ' ')) {
            valueStart++;
        }

        if (valueStart < cmdLine.length()) {
            launch.deferredLaunch = cmdLine.substr(valueStart);

            // Game launch will happen in ControllerMain AFTER child processes are
            // ready
            if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
                Log_Init(earlyLogsDir + "\\launcher.log", RuntimeConfiguration().logLevel);
                LogInfo("[Launcher] Deferred launch path: %s", launch.deferredLaunch.c_str());
            }

            // Continue as Controller
            mode = ProcessMode::Controller;
        }
    }

    // Setup logging with process-specific log file in session logs subfolder
    std::string logsDir = earlyLogsDir;
    const std::string processLogName = GetProcessLogFileName(mode, g_RecordingId, GetCurrentProcessId());
    std::string logPath = logsDir + "\\" + processLogName;
    ce::runtime::SetRuntimeProcessLogPath(logPath);
    if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
        CreateDirectoryA(logsDir.c_str(), NULL);
        if (mode == ProcessMode::Controller)
            WriteSessionManifest(logsDir, RuntimeConfiguration(), mode);
        else if (mode == ProcessMode::Media)
            WriteRecordingManifest(logsDir, RuntimeConfiguration(), processLogName);
    }

    if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
        Log_Init(logPath, RuntimeConfiguration().logLevel);
        LogInfo("CaptureEngine Starting... Version: %s (Built: %s)", GetCaptureVersion(), GetBuildTimestamp());
        if (!runtimePathsExact) {
            LogWarn(
                "[Controller] A runtime or configuration path cannot be expressed in the Windows code page and has no 8.3 "
                "short name; config.ini and logs may not be found. Install CaptureEngine to a folder with Latin "
                "characters or enable 8.3 names on that volume");
        }
        LogInfo("Process Mode: %s", mode == ProcessMode::Controller ? "Controller"
                                    : mode == ProcessMode::Inject   ? "Inject"
                                    : mode == ProcessMode::Media    ? "Media"
                                    : mode == ProcessMode::Logger   ? "Logger"
                                    : mode == ProcessMode::Sensors  ? "Sensors"
                                                                    : "Unknown");
    }

    // Opt out of Windows 11 EcoQoS / power throttling for all sub-processes.
    // This tells the scheduler to prefer P-cores over E-cores on hybrid CPUs.
    PROCESS_POWER_THROTTLING_STATE pts = {};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pts.StateMask = 0;  // 0 = disable throttling (prefer performance cores)
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pts, sizeof(pts));

    // Controller: Single instance check
    if (mode == ProcessMode::Controller) {
        // If restarted from a prior instance, wait for that instance to exit cleanly
        // so its single-instance mutex and ports are fully released.
        const char* fullCommandLine = GetCommandLineA();
        const char* restartArg = strstr(fullCommandLine, "--restart-from-pid=");
        if (restartArg) {
            uint32_t priorPid = 0;
            const char* val = restartArg + 19;
            const char* end = val;
            while (*end >= '0' && *end <= '9')
                ++end;
            if (ce::TryParseUInt32(std::string_view(val, static_cast<size_t>(end - val)), priorPid) && priorPid != 0) {
                HANDLE hPrior = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, priorPid);
                if (hPrior) {
                    LogInfo("[Controller] Waiting for prior instance (PID %u) to exit...", priorPid);
                    const DWORD waitResult = WaitForSingleObject(hPrior, 5000);
                    if (waitResult == WAIT_TIMEOUT) {
                        LogWarn("[Controller] Prior instance did not exit in 5s; terminating it");
                        TerminateProcess(hPrior, 0);
                        WaitForSingleObject(hPrior, 1000);
                    }
                    CloseHandle(hPrior);
                }
            }
        }

        HANDLE hMutex = nullptr;
        for (int retry = 0; retry < 20; ++retry) {
            hMutex = CreateMutexA(0, FALSE, "Local\\CaptureEngine_Instance_Mutex");
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                break;
            if (hMutex) {
                CloseHandle(hMutex);
                hMutex = nullptr;
            }
            Sleep(50);
        }
        if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxA(NULL, "CaptureEngine is already running.", "Error", MB_ICONERROR);
            return 1;
        }
    }

    // Keep crash dumps under logs/. Config can only add a relative subfolder.
    std::string crashDir = logsDir;
    if (!RuntimeConfiguration().crashDumpDir.empty()) {
        std::filesystem::path configured = std::filesystem::path(RuntimeConfiguration().crashDumpDir).lexically_normal();
        bool hasParentTraversal = false;
        for (const auto& part : configured) {
            if (part == std::filesystem::path("..")) {
                hasParentTraversal = true;
                break;
            }
        }
        if (!configured.empty() && configured != "." && !configured.is_absolute() && !hasParentTraversal) {
            crashDir = (std::filesystem::path(logsDir) / configured).string();
        }
    }
    if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
        SetCrashDumpDirectory(crashDir);
        if (mode == ProcessMode::Controller) {
            // After this session linked its own symbols and old sessions were
            // pruned: a store file nothing links anymore is no longer needed.
            PruneCrashSymbolStore();
        }
    }

    // Dispatch to appropriate process
    int result = 0;
    switch (mode) {
        case ProcessMode::Controller:
            result = ControllerMain(hInstance, *paths, launch);
            break;
        case ProcessMode::Inject:
            result = InjectProcessMain(RuntimeConfiguration());
            break;
        case ProcessMode::Media:
            result = MediaProcessMain(RuntimeConfiguration());
            break;
        case ProcessMode::Logger:
            result = LoggerProcessMain(RuntimeConfiguration());
            break;
        case ProcessMode::Sensors:
            result = SensorProcessMain(RuntimeConfiguration());
            break;
    }

    // Cleanup
    Log_Shutdown();

    return result;
}
