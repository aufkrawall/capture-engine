#include "main_internal.h"

#include "frontend/app/frontend_main.h"
#include "common/platform/path_utils.h"
#include "captureengine/elevation/startup_control.h"

namespace {
// `--launch <command>` (or `--launch=<command>`) with a command behind it starts the app, also when the game's own
// arguments behind it contain a --mode= (the Steam `--launch %command%` form).
bool RequestsGameLaunch(const char* commandLine) {
    const std::string_view text(commandLine ? commandLine : "");
    const size_t flag = text.find("--launch");
    if (flag == std::string_view::npos)
        return false;
    size_t value = flag + 8;
    while (value < text.size() && (text[value] == '=' || text[value] == ' '))
        ++value;
    return value < text.size();
}
}  // namespace

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
    if (RequestsGameLaunch(lpCmdLine))
        mode = ProcessMode::Controller;
    if (mode == ProcessMode::Controller) {
        // The app (llm-wiki/library/05) drives the runtime only through its C API. Settings, the session
        // directory, logging, crash handling and the one-runtime-per-session claim belong to the runtime it
        // creates, so nothing below runs in this process.
        const bool informational =
            HasExactCommandLineArgument(L"--list-monitors") || HasExactCommandLineArgument(L"--license");
        if (const auto startupResult = ce::startup::Bootstrap(!informational))
            return *startupResult;
        if (HasExactCommandLineArgument(L"--list-monitors"))
            return ce::monitor_selection::WriteMonitorListToStandardOutput();
        return FrontendMain(hInstance);
    }

    // Helper roles from here on. Get paths. Config, logs and crash handling open files through the ANSI
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

    // Load config early so directory and crash-handler setup can be gated on
    // the configured log_level. When log_level=none/off we skip everything to
    // guarantee the logs/ tree stays absent and no debug machinery runs.
    ce::runtime::RuntimeConfigurationSession configuration(paths->Configuration());
    if (!configuration.IsReady()) {
        OutputDebugStringA("[CaptureEngine] Cannot acquire runtime configuration ownership\n");
        return 1;
    }

    std::string logsRootDir = baseDir + "\\logs";
    // Session symbol archives are hard links into one shared store below the
    // logs root instead of a ~180 MB copy per session (see crash_symbol_store.h).
    SetCrashSymbolStoreRoot(logsRootDir);

    // The runtime names the session directory; helpers inherit it from --session-dir= on the command line.
    g_SessionDirName = ParseSessionDir(lpCmdLine);
    g_RecordingId = ParseRecordingId(lpCmdLine);

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

    // Setup logging with process-specific log file in session logs subfolder
    std::string logsDir = earlyLogsDir;
    const std::string processLogName = GetProcessLogFileName(mode, g_RecordingId, GetCurrentProcessId());
    std::string logPath = logsDir + "\\" + processLogName;
    ce::runtime::SetRuntimeProcessLogPath(logPath);
    if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
        CreateDirectoryA(logsDir.c_str(), NULL);
        if (mode == ProcessMode::Media)
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
        LogInfo("Process Mode: %s", mode == ProcessMode::Inject   ? "Inject"
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
    }

    // Dispatch to appropriate process
    int result = 0;
    switch (mode) {
        case ProcessMode::Controller:
            break;  // returned to FrontendMain above
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
