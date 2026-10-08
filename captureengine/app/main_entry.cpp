#include "main_internal.h"

#include "libcaptureengine_controller.h"
#include "common/platform/path_utils.h"
#include "common/platform/window_heartbeat.h"
#include "captureengine/elevation/startup_control.h"
#include "captureengine/sensors/pawnio_workers.h"

#include <algorithm>

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LogInfo("[Controller] Console event %lu received. Cleaning up...", ctrlType);
        main_g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Acts on one hotkey, whichever path delivered it. RegisterHotKey posts
// WM_HOTKEY; the low-level keyboard hook posts main_kMsgHotkeyFromInputHook for
// the applications that suppress hotkey processing entirely. Exactly one of the
// two ever fires for a press, because the hook consumes the key it matched.
void DispatchHotkey(int hotkeyId) {
    if (hotkeyId == HOTKEY_ID_RECORD) {
        ToggleRecording();
        return;
    }
    if (hotkeyId == HOTKEY_ID_AUDIO_ONLY) {
        ToggleAudioOnlyRecording();
        return;
    }
    if (hotkeyId == HOTKEY_ID_TOGGLE_OVERLAY) {
        ToggleOverlay();
        return;
    }
    if (hotkeyId == HOTKEY_ID_BENCHMARK) {
        ToggleBenchmark();
        return;
    }
    if (hotkeyId != HOTKEY_ID_SCREENSHOT)
        return;

    ce_engine_take_screenshot(GetControllerEngine());
}

void DispatchControllerMessage(const MSG& msg) {
    if (msg.message == WM_QUIT) {
        main_g_Running = false;
        return;
    }
    if (msg.message == main_kMsgCompleteControllerStartup) {
        if (!CompleteControllerStartup()) {
            ShutdownChildProcesses();
            main_g_Running = false;
        } else {
            // Offered once startup is complete so the prompt cannot
            // delay child spawning, and on its own thread so it cannot
            // swallow hotkeys from this loop.
            ce::pawnio::OfferInstallationAsync(RuntimeConfiguration().hardwareSensors);
        }
        return;
    }
    if (msg.message == main_kMsgHotkeyFromInputHook) {
        LogDebug("[Hotkey] Keyboard-hook delivery id=%d vk=0x%02X total=%llu", static_cast<int>(msg.wParam),
                 static_cast<unsigned>(msg.lParam),
                 static_cast<unsigned long long>(GetHotkeyInputHookDeliveredCount()));
    }
    if (msg.message == WM_HOTKEY || msg.message == main_kMsgHotkeyFromInputHook) {
        DispatchHotkey(static_cast<int>(msg.wParam));
        return;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
}

// Controller main function
int ControllerMain(HINSTANCE hInstance, const ce::runtime::RuntimePackagePaths& paths) {
    ControllerRecordingSessionScope recordingSession;
    const int64_t controllerStartUs = Log_GetQpcUs();
    LogInfo("[Controller] Starting...");
    ce::window_heartbeat::Service windowHeartbeat;
    windowHeartbeat.UpdateProfiles(RuntimeConfiguration().applicationProfiles);
    PrimeStartupCursor();

    SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);

    // Give Explorer a real UI owner before registration or child startup can
    // extend the launch-feedback cursor. The window remains hidden and inactive.
    LogInfo("[Controller] Creating tray icon...");
    const int64_t trayCreateStartUs = Log_GetQpcUs();
    TrayIcon::Callbacks trayCallbacks;
    trayCallbacks.onQuit = []() { main_g_Running = false; };
    trayCallbacks.onOpenConfig = []() {
        ShellExecuteA(NULL, "open", main_g_ConfigPath.c_str(), NULL, NULL, SW_SHOW);
    };
    trayCallbacks.onInstallPawnIo = []() { ce::pawnio::InstallDriverAsync(); };
    trayCallbacks.onUninstallPawnIo = []() {
        if (!ce::runtime::StopHostSensorsForSetup()) {
            LogError("[PawnIO] Cannot uninstall driver before the sensor process has exited");
            return;
        }
        ce::pawnio::UninstallDriverAsync();
    };
    trayCallbacks.isPawnIoInstalled = []() { return ce::pawnio::IsDriverInstalled(); };
    trayCallbacks.onToggleStartup = ce::startup::Toggle;
    trayCallbacks.startupPreferences = ce::startup::DisplayPreferences;
    trayCallbacks.elevationServiceStatus = ce::startup::ServiceStatusText;
    trayCallbacks.startupBusy = ce::startup::Busy;
    auto tray = std::make_unique<TrayIcon>(hInstance, std::move(trayCallbacks));
    const int64_t trayCreateUs = Log_GetQpcUs() - trayCreateStartUs;
    main_g_Tray = tray.get();
    PrimeStartupCursor();
    PumpStartupMessages();

    // Resident registration: intentionally outlives this process so a Vulkan
    // title started before CaptureEngine still carries the layer and can be
    // injected late.
    const int64_t vulkanRegStartUs = Log_GetQpcUs();
    VulkanLayerResidency vulkanReg;
    const int64_t vulkanRegUs = Log_GetQpcUs() - vulkanRegStartUs;
    if (!vulkanReg.IsActive()) {
        LogWarn(
            "[Controller] Vulkan layer registration is inactive; Vulkan capture may be unavailable for this session");
    }

    // Create IPC clients
    ce::runtime::HostChildrenSession children(main_g_ConfigPath.c_str(), PumpStartupMessages,
                                             []() { return main_g_Running.load(); }, paths.Executable().c_str());
    if (!children.IsReady()) {
        LogError("[Controller] Failed to acquire runtime child ownership");
        main_g_Running = false;
    }

    main_g_ControllerStartupTiming.controllerStartUs = controllerStartUs;
    main_g_ControllerStartupTiming.vulkanRegUs = vulkanRegUs;
    main_g_ControllerStartupTiming.trayCreateUs = trayCreateUs;
    main_g_ControllerStartupTiming.complete = false;
    PostThreadMessage(GetCurrentThreadId(), main_kMsgCompleteControllerStartup, 0, 0);

    ControllerApiSession controllerApi;
    if (!controllerApi.IsReady()) {
        LogError("[Controller] Failed to initialize controller API");
        main_g_Running = false;
    }

    // Main message loop
    MSG msg;

    // Controller loop health: one summary per minute, plus an immediate line when a single
    // iteration's own work (messages + health + config) blocks this UI thread for 100 ms or more.
    constexpr int64_t kLoopSummaryWindowUs = 60'000'000;
    constexpr int64_t kSlowIterationUs = 100'000;
    struct LoopWindow {
        int64_t startUs = 0;
        uint64_t iterations = 0;
        uint64_t messages = 0;
        uint64_t hotkeys = 0;
        uint64_t zeroWaits = 0;
        int64_t maxMsgUs = 0;
        int64_t maxHealthUs = 0;
        int64_t maxConfigUs = 0;
    };
    static LoopWindow loopWindow{Log_GetQpcUs()};
    static uint64_t iterCount = 0;

    while (main_g_Running) {
        ce::startup::Pump();
        iterCount++;
        const int64_t iterNowUs = Log_GetQpcUs();

        // Process messages
        int msgCount = 0;
        int msgTimers = 0;
        int msgOthers = 0;
        int msgHotkeys = 0;
        int msgHookHotkeys = 0;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            msgCount++;
            if (msg.message == WM_TIMER)
                msgTimers++;
            else if (msg.message == WM_HOTKEY)
                msgHotkeys++;
            else if (msg.message == main_kMsgHotkeyFromInputHook)
                msgHookHotkeys++;
            else if (msg.message != WM_QUIT && msg.message != main_kMsgCompleteControllerStartup)
                msgOthers++;
            DispatchControllerMessage(msg);
        }

        const int64_t postMsgUs = Log_GetQpcUs();

        ReportHotkeyInputHookDiagnostics();

        // A fatal media transport failure must clear controller ownership before
        // health recovery can mistake the intentional failed stop for a crash.
        CheckRecordingFailureState();

        // Check child process health
        CheckChildProcessHealth();

        const int64_t postHealthUs = Log_GetQpcUs();

        // The runtime owns read coherence, replacement and retry scheduling.
        if (auto previous = ce::runtime::PollRuntimeConfiguration()) {
            LogInfo("[Controller] Applying published configuration to frontend/services");
            const AppConfig& oldConfig = *previous;
            Log_SetLevel(RuntimeConfiguration().logLevel);
            windowHeartbeat.UpdateProfiles(RuntimeConfiguration().applicationProfiles);

            if (!HotkeyConfigEquals(oldConfig.hotkeyStartStop, RuntimeConfiguration().hotkeyStartStop)) {
                UnregisterHotKey(NULL, HOTKEY_ID_RECORD);
                main_g_HotkeyOwnership.record =
                    RegisterConfiguredHotkey(HOTKEY_ID_RECORD, RuntimeConfiguration().hotkeyStartStop, "recording");
            }

            if (!HotkeyConfigEquals(oldConfig.hotkeyScreenshot, RuntimeConfiguration().hotkeyScreenshot)) {
                UnregisterHotKey(NULL, HOTKEY_ID_SCREENSHOT);
                main_g_HotkeyOwnership.screenshot =
                    RegisterConfiguredHotkey(HOTKEY_ID_SCREENSHOT, RuntimeConfiguration().hotkeyScreenshot,
                                             "screenshot");
            }

            if (!HotkeyConfigEquals(oldConfig.hotkeyAudioOnly, RuntimeConfiguration().hotkeyAudioOnly)) {
                UnregisterHotKey(NULL, HOTKEY_ID_AUDIO_ONLY);
                main_g_HotkeyOwnership.audioOnly =
                    RegisterConfiguredHotkey(HOTKEY_ID_AUDIO_ONLY, RuntimeConfiguration().hotkeyAudioOnly,
                                             "audio-only");
            }

            if (!HotkeyConfigEquals(oldConfig.hotkeyToggleOverlay, RuntimeConfiguration().hotkeyToggleOverlay)) {
                UnregisterHotKey(NULL, HOTKEY_ID_TOGGLE_OVERLAY);
                main_g_HotkeyOwnership.toggleOverlay =
                    RegisterConfiguredHotkey(HOTKEY_ID_TOGGLE_OVERLAY, RuntimeConfiguration().hotkeyToggleOverlay,
                                             "overlay toggle");
            }

            if (!HotkeyConfigEquals(oldConfig.hotkeyBenchmark, RuntimeConfiguration().hotkeyBenchmark)) {
                UnregisterHotKey(NULL, HOTKEY_ID_BENCHMARK);
                main_g_HotkeyOwnership.benchmark =
                    RegisterConfiguredHotkey(HOTKEY_ID_BENCHMARK, RuntimeConfiguration().hotkeyBenchmark,
                                             "benchmark");
            }

            // The keyboard-hook path recognizes the same hotkeys, so it
            // has to follow every reload, including one that only
            // disabled a hotkey.
            PublishHotkeyBindings(RuntimeConfiguration(), main_g_HotkeyOwnership);

            {
                MainThreadBlockTimer _blk("config-reload service sync");
                SyncLoggerAndSensorProcesses(RuntimeConfiguration(), &oldConfig);
                SendCommandToAll(ProcessCommand::ReloadConfig);
            }

            SyncPseudoOverlayConfiguration("config reload");
        }

        // Auto-record logic
        if (main_g_AutoRecordEnabled && main_g_AutoRecordStartTime > 0) {
            DWORD elapsed = GetTickCount() - main_g_AutoRecordStartTime;
            if (!ControllerRecordingSnapshot().requested && elapsed >= main_g_AutoRecordDelayMs) {
                LogInfo("[Controller] Auto-record: starting recording...");
                ToggleRecording();
            } else if (ControllerRecordingSnapshot().requested && elapsed >= (main_g_AutoRecordDelayMs + main_g_AutoRecordDurationMs)) {
                LogInfo("[Controller] Auto-record: stopping recording...");
                ToggleRecording();
                main_g_Running = false;  // Exit after auto-record completes
            }
        }

        const int64_t preWaitUs = Log_GetQpcUs();

        const DWORD waitMs = GetControllerLoopWaitMs();

        {
            const int64_t msgUs = postMsgUs - iterNowUs;
            const int64_t healthUs = postHealthUs - postMsgUs;
            const int64_t configUs = preWaitUs - postHealthUs;
            if (msgUs + healthUs + configUs >= kSlowIterationUs) {
                LogDebug("[ControllerDiag] slow iteration %llu: msg=%lldus health=%lldus config=%lldus messages=%d "
                         "(timer=%d other=%d hk=%d hkHook=%d)",
                         (unsigned long long)iterCount, (long long)msgUs, (long long)healthUs, (long long)configUs,
                         msgCount, msgTimers, msgOthers, msgHotkeys, msgHookHotkeys);
            }
            loopWindow.iterations++;
            loopWindow.messages += static_cast<uint64_t>(msgCount);
            loopWindow.hotkeys += static_cast<uint64_t>(msgHotkeys + msgHookHotkeys);
            loopWindow.zeroWaits += waitMs == 0 ? 1 : 0;
            loopWindow.maxMsgUs = (std::max)(loopWindow.maxMsgUs, msgUs);
            loopWindow.maxHealthUs = (std::max)(loopWindow.maxHealthUs, healthUs);
            loopWindow.maxConfigUs = (std::max)(loopWindow.maxConfigUs, configUs);
            const int64_t windowUs = preWaitUs - loopWindow.startUs;
            if (windowUs >= kLoopSummaryWindowUs) {
                LogDebug("[ControllerDiag] %llds: iterations=%llu messages=%llu hotkeys=%llu zeroWaits=%llu "
                         "max(msg=%lldus health=%lldus config=%lldus) hotkeyHook=%d",
                         (long long)(windowUs / 1000000), (unsigned long long)loopWindow.iterations,
                         (unsigned long long)loopWindow.messages, (unsigned long long)loopWindow.hotkeys,
                         (unsigned long long)loopWindow.zeroWaits, (long long)loopWindow.maxMsgUs,
                         (long long)loopWindow.maxHealthUs, (long long)loopWindow.maxConfigUs,
                         IsHotkeyInputHookActive() ? 1 : 0);
                loopWindow = LoopWindow{preWaitUs};
            }
        }

        MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, 0);
    }

    // Stop the independent heartbeat before controller teardown.
    windowHeartbeat.Stop();
    // Unregister hotkeys first
    StopHotkeyInputHook();
    UnregisterHotKey(NULL, HOTKEY_ID_RECORD);
    UnregisterHotKey(NULL, HOTKEY_ID_SCREENSHOT);
    UnregisterHotKey(NULL, HOTKEY_ID_AUDIO_ONLY);
    UnregisterHotKey(NULL, HOTKEY_ID_TOGGLE_OVERLAY);
    UnregisterHotKey(NULL, HOTKEY_ID_BENCHMARK);

    // Keep tray icon alive during shutdown (animation already started by
    // right-click handler) Process messages during shutdown so animation
    // continues
    if (main_g_Tray) {
        main_g_Tray->StartShutdownAnimation();
    }

    // Shutdown pseudo-overlay before child processes
    if (main_g_PseudoOverlay) {
        main_g_PseudoOverlay->Shutdown();
        main_g_PseudoOverlay.reset();
    }

    ce::startup::Shutdown();
    ce::pawnio::ShutdownSetupWorkers();
    ShutdownChildProcesses();

    // Now remove tray icon after shutdown is complete
    if (tray) {
        tray->Remove();
    }
    main_g_Tray = nullptr;
    tray.reset();

    LogInfo("[Controller] Exiting");
    return controllerApi.IsReady() ? 0 : 1;
}

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
    main_g_ConfigPath = paths->Configuration();

    // Load config early so directory and crash-handler setup can be gated on
    // the configured log_level. When log_level=none/off we skip everything to
    // guarantee the logs/ tree stays absent and no debug machinery runs.
    ce::runtime::RuntimeConfigurationSession configuration(main_g_ConfigPath);
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
                main_g_AutoRecordDelayMs = delayMs;
                main_g_AutoRecordDurationMs = durationMs;
                main_g_AutoRecordEnabled = true;
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
            main_g_DeferredLaunchPath = cmdLine.substr(valueStart);

            // Game launch will happen in ControllerMain AFTER child processes are
            // ready
            if (IsAnyLoggingEnabled(RuntimeConfiguration().logLevel)) {
                Log_Init(earlyLogsDir + "\\launcher.log", RuntimeConfiguration().logLevel);
                LogInfo("[Launcher] Deferred launch path: %s", main_g_DeferredLaunchPath.c_str());
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
            result = ControllerMain(hInstance, *paths);
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
