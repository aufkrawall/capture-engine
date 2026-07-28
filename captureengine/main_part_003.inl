    RegisterHotKey(NULL, HOTKEY_ID_RECORD, g_Config.hotkeyStartStop.GetModifiers(), g_Config.hotkeyStartStop.vkey);
    if (g_Config.hotkeyScreenshot.vkey != 0) {
        RegisterHotKey(NULL, HOTKEY_ID_SCREENSHOT, g_Config.hotkeyScreenshot.GetModifiers(),
                       g_Config.hotkeyScreenshot.vkey);
    }
    if (g_Config.hotkeyAudioOnly.vkey != 0) {
        RegisterHotKey(NULL, HOTKEY_ID_AUDIO_ONLY, g_Config.hotkeyAudioOnly.GetModifiers(),
                       g_Config.hotkeyAudioOnly.vkey);
    }
    const int64_t hotkeyUs = Log_GetQpcUs() - hotkeyStartUs;

    SyncPseudoOverlayConfiguration("startup");

    LogInfo("[Controller] Ready. Press hotkey to start recording.");
    PrimeStartupCursor();
    LogInfo(
        "[StartupPerf] Controller startup: VulkanRegistration=%.3f ms, SpawnInject=%.3f ms, "
        "SpawnMedia=%.3f ms, SpawnLimiter=%.3f ms, SpawnAux=%.3f ms, IPCConnect=%.3f ms, TrayCreate=%.3f ms, "
        "RegisterHotkeys=%.3f ms, TotalToReady=%.3f ms",
        QpcDeltaToMs(g_ControllerStartupTiming.vulkanRegUs), QpcDeltaToMs(injectSpawnUs), QpcDeltaToMs(mediaSpawnUs),
        QpcDeltaToMs(limiterSpawnUs), QpcDeltaToMs(auxSpawnUs), QpcDeltaToMs(ipcConnectUs),
        QpcDeltaToMs(g_ControllerStartupTiming.trayCreateUs), QpcDeltaToMs(hotkeyUs),
        QpcDeltaToMs(Log_GetQpcUs() - g_ControllerStartupTiming.controllerStartUs));

    if (g_AutoRecordEnabled) {
        LogInfo("[Controller] Auto-record enabled: delay=%lums, duration=%lums", g_AutoRecordDelayMs,
                g_AutoRecordDurationMs);
        g_AutoRecordStartTime = GetTickCount();
    }

    g_ControllerStartupTiming.complete = true;
    return true;
}

static ce::vulkan_layer::RegistrationPlan BuildControllerVulkanRegistrationPlan() {
    std::filesystem::path baseDir;
    if (!ce::vulkan_layer::GetCurrentExecutableDirectory(&baseDir)) {
        LogError("[Controller] Failed to resolve executable directory for Vulkan layer registration");
        return {};
    }

    return ce::vulkan_layer::BuildRegistrationPlan(baseDir, ce::vulkan_layer::RegistrationMode::Auto,
                                                   ce::vulkan_layer::IsCurrentProcessElevated());
}

// RAII wrapper for exact registration ownership. Startup repairs superseded CE
// entries only in registry scopes already writable by this process; the retained
// plan then lets teardown remove only this instance's exact registrations.
class ScopedVulkanRegistration {
public:
    ScopedVulkanRegistration() : plan_(BuildControllerVulkanRegistrationPlan()) {
        ce::vulkan_layer::LogRegistrationPlan(plan_);
        if (!ce::vulkan_layer::RepairOwnedRegistrations(plan_)) {
            LogWarn("[Controller] Vulkan layer registration repair was incomplete");
        }
        active_ = ce::vulkan_layer::ApplyRegistrationPlan(plan_, true);
        if (!active_) {
            LogError("[Controller] Vulkan layer registration failed");
        }
    }
    ~ScopedVulkanRegistration() {
        Unregister();
    }

    bool IsActive() const {
        return active_;
    }

    void Unregister() {
        std::call_once(unregistrationOnce_, [this]() {
            if (!ce::vulkan_layer::ApplyRegistrationPlan(plan_, false)) {
                LogError("[Controller] Vulkan layer unregistration failed");
            }
        });
    }

private:
    ce::vulkan_layer::RegistrationPlan plan_;
    std::once_flag unregistrationOnce_;
    bool active_ = false;
};

// Global pointer for emergency unregistration
static ScopedVulkanRegistration* g_VulkanReg = nullptr;

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        LogInfo("[Controller] Console event %lu received. Cleaning up...", ctrlType);
        if (g_VulkanReg) {
            g_VulkanReg->Unregister();
        }
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Controller main function
int ControllerMain(HINSTANCE hInstance) {
    const int64_t controllerStartUs = Log_GetQpcUs();
    LogInfo("[Controller] Starting...");
    PrimeStartupCursor();

    SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);

    // Give Explorer a real UI owner before registration or child startup can
    // extend the launch-feedback cursor. The window remains hidden and inactive.
    LogInfo("[Controller] Creating tray icon...");
    const int64_t trayCreateStartUs = Log_GetQpcUs();
    auto tray = std::make_unique<TrayIcon>(
        hInstance, []() { g_Running = false; },
        []() { ShellExecuteA(NULL, "open", g_ConfigPath.c_str(), NULL, NULL, SW_SHOW); });
    const int64_t trayCreateUs = Log_GetQpcUs() - trayCreateStartUs;
    g_Tray = tray.get();
    PrimeStartupCursor();
    PumpStartupMessages();

    // Ephemeral Registration (RAII)
    const int64_t vulkanRegStartUs = Log_GetQpcUs();
    ScopedVulkanRegistration vulkanReg;
    const int64_t vulkanRegUs = Log_GetQpcUs() - vulkanRegStartUs;
    g_VulkanReg = &vulkanReg;
    if (!vulkanReg.IsActive()) {
        LogWarn(
            "[Controller] Vulkan layer registration is inactive; Vulkan capture may be unavailable for this session");
    }

    // Create IPC clients
    g_InjectClient = std::make_unique<ProcessIPCClient>(ProcessMode::Inject);
    g_MediaClient = std::make_unique<ProcessIPCClient>(ProcessMode::Media);
    g_LimiterClient = std::make_unique<ProcessIPCClient>(ProcessMode::Limiter);

    g_ControllerStartupTiming.controllerStartUs = controllerStartUs;
    g_ControllerStartupTiming.vulkanRegUs = vulkanRegUs;
    g_ControllerStartupTiming.trayCreateUs = trayCreateUs;
    g_ControllerStartupTiming.complete = false;
    PostThreadMessage(GetCurrentThreadId(), kMsgCompleteControllerStartup, 0, 0);

    // Main message loop
    MSG msg;
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // Diagnostic loop timing
    static int64_t loopStartUs = Log_GetQpcUs();
    static uint64_t iterCount = 0;
    static uint64_t iterRateLogCount = 0;
    static int64_t iterRateLogStartUs = Log_GetQpcUs();
    static DWORD lastConfigCheck = 0;

    while (g_Running) {
        iterCount++;
        const int64_t iterNowUs = Log_GetQpcUs();
        const int64_t iterDeltaUs = iterNowUs - loopStartUs;
        loopStartUs = iterNowUs;

        // Log iteration rate every ~5s at trace level
        iterRateLogCount++;
        const int64_t rateLogElapsedUs = iterNowUs - iterRateLogStartUs;
        if (rateLogElapsedUs > 5000000) {
            const double rateHz = static_cast<double>(iterRateLogCount) / (rateLogElapsedUs / 1000000.0);
            LogDebug("[ControllerDiag] iter=%llu rate=%.1f Hz delta=%lld us waitMs=%lu msgProc=%d",
                     (unsigned long long)iterCount, rateHz, (long long)iterDeltaUs,
                     GetControllerLoopWaitMs(lastConfigCheck), 0);
            iterRateLogCount = 0;
            iterRateLogStartUs = iterNowUs;
        }

        // Process messages
        int msgCount = 0;
        int msgTimers = 0;
        int msgOthers = 0;
        int msgHotkeys = 0;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            msgCount++;
            if (msg.message == WM_TIMER)
                msgTimers++;
            else if (msg.message == WM_HOTKEY)
                msgHotkeys++;
            else if (msg.message != WM_QUIT && msg.message != kMsgCompleteControllerStartup)
                msgOthers++;
            if (msg.message == WM_QUIT) {
                g_Running = false;
                continue;
            }
            if (msg.message == kMsgCompleteControllerStartup) {
                if (!CompleteControllerStartup()) {
                    ShutdownChildProcesses();
                    g_Running = false;
                }
                continue;
            }
            if (msg.message == WM_HOTKEY) {
                if (msg.wParam == HOTKEY_ID_RECORD) {
                    ToggleRecording();
                } else if (msg.wParam == HOTKEY_ID_AUDIO_ONLY) {
                    ToggleAudioOnlyRecording();
                } else if (msg.wParam == HOTKEY_ID_SCREENSHOT) {
                    if (g_PseudoOverlay)
                        g_PseudoOverlay->BeginScreenshotCapture();
                    const bool screenshotSaved =
                        TakeScreenshot(g_Config.screenshotDir, g_Config.screenshotColorSpace);
                    if (g_PseudoOverlay) {
                        g_PseudoOverlay->EndScreenshotCapture();
                        g_PseudoOverlay->ShowScreenshotNotification(screenshotSaved);
                    }
                    // Show the same result in the inject overlay (hooked game).
                    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
                    if (hDisc) {
                        DiscoveryInfo* pDisc =
                            (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
                        if (ValidateDiscoveryInfo(pDisc)) {
                            uint32_t injPid = pDisc->GetInjectPid();
                            UnmapViewOfFile(pDisc);
                            CloseHandle(hDisc);
                            if (injPid != 0) {
                                wchar_t shmName[64];
                                GenerateSharedMemName(shmName, 64, injPid);
                                HANDLE hShm = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, shmName);
                                if (hShm) {
                                    auto* pShm = (SharedMemoryLayout*)MapViewOfFile(
                                        hShm, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout));
                                    if (pShm && ValidateSharedMemory(pShm)) {
                                        const OverlayNotificationType notification =
                                            screenshotSaved ? OverlayNotificationType::ScreenshotSaved
                                                            : OverlayNotificationType::ScreenshotFailed;
                                        pShm->runtimeState.notificationType.store(static_cast<uint32_t>(notification),
                                                                                 std::memory_order_release);
                                        pShm->runtimeState.notificationExpiry.store(GetTickCount64() + 2000ULL,
                                                                                    std::memory_order_release);
                                    } else if (pShm) {
                                        LogError("[Controller] Screenshot notification rejected incompatible "
                                                 "inject shared memory ABI");
                                    }
                                    if (pShm)
                                        UnmapViewOfFile(pShm);
                                    CloseHandle(hShm);
                                }
                            }
                        } else {
                            if (pDisc)
                                UnmapViewOfFile(pDisc);
                            CloseHandle(hDisc);
                        }
                    }
                }
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        const int64_t postMsgUs = Log_GetQpcUs();

        // A fatal media transport failure must clear controller ownership before
        // health recovery can mistake the intentional failed stop for a crash.
        CheckRecordingFailureState();

        // Check child process health
        CheckChildProcessHealth();

        const int64_t postHealthUs = Log_GetQpcUs();

        // Config hot-reload
        DWORD configNow = GetTickCount();
        if (configNow - lastConfigCheck >= 1000) {
            WIN32_FILE_ATTRIBUTE_DATA fileInfo;
            if (GetFileAttributesExA(g_ConfigPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
                static FILETIME lastWriteTime = fileInfo.ftLastWriteTime;
                if (CompareFileTime(&fileInfo.ftLastWriteTime, &lastWriteTime) > 0) {
                    LogInfo("[Controller] Config change detected, reloading...");
                    lastWriteTime = fileInfo.ftLastWriteTime;

                    AppConfig oldConfig = g_Config;
                    LoadConfig(g_ConfigPath, g_Config);
                    Log_SetLevel(g_Config.logLevel);

                    if (!HotkeyConfigEquals(oldConfig.hotkeyStartStop, g_Config.hotkeyStartStop)) {
                        UnregisterHotKey(NULL, HOTKEY_ID_RECORD);
                        if (!RegisterHotKey(NULL, HOTKEY_ID_RECORD, g_Config.hotkeyStartStop.GetModifiers(),
                                            g_Config.hotkeyStartStop.vkey)) {
                            LogError("[Controller] Failed to re-register recording hotkey");
                        }
                    }

                    if (!HotkeyConfigEquals(oldConfig.hotkeyScreenshot, g_Config.hotkeyScreenshot)) {
                        UnregisterHotKey(NULL, HOTKEY_ID_SCREENSHOT);
                        if (g_Config.hotkeyScreenshot.vkey != 0) {
                            if (!RegisterHotKey(NULL, HOTKEY_ID_SCREENSHOT, g_Config.hotkeyScreenshot.GetModifiers(),
                                                g_Config.hotkeyScreenshot.vkey)) {
                                LogError("[Controller] Failed to re-register screenshot hotkey");
                            }
                        }
                    }

                    if (!HotkeyConfigEquals(oldConfig.hotkeyAudioOnly, g_Config.hotkeyAudioOnly)) {
                        UnregisterHotKey(NULL, HOTKEY_ID_AUDIO_ONLY);
                        if (g_Config.hotkeyAudioOnly.vkey != 0) {
                            if (!RegisterHotKey(NULL, HOTKEY_ID_AUDIO_ONLY, g_Config.hotkeyAudioOnly.GetModifiers(),
                                                g_Config.hotkeyAudioOnly.vkey)) {
                                LogError("[Controller] Failed to re-register audio-only hotkey");
                            }
                        }
                    }

                    {
                        MainThreadBlockTimer _blk("config-reload service sync");
                        SyncLoggerAndSensorProcesses(g_Config);
                        SyncLimiterProcess(g_Config);
                        SendCommandToAll(ProcessCommand::ReloadConfig);
                    }

                    SyncPseudoOverlayConfiguration("config reload");
                }
            }
            lastConfigCheck = GetTickCount();
        }

        // Auto-record logic
        if (g_AutoRecordEnabled && g_AutoRecordStartTime > 0) {
            DWORD elapsed = GetTickCount() - g_AutoRecordStartTime;
            if (!g_Recording && elapsed >= g_AutoRecordDelayMs) {
                LogInfo("[Controller] Auto-record: starting recording...");
                ToggleRecording();
            } else if (g_Recording && elapsed >= (g_AutoRecordDelayMs + g_AutoRecordDurationMs)) {
                LogInfo("[Controller] Auto-record: stopping recording...");
                ToggleRecording();
                g_Running = false;  // Exit after auto-record completes
            }
        }

        const int64_t preWaitUs = Log_GetQpcUs();

        const DWORD waitMs = GetControllerLoopWaitMs(lastConfigCheck);

        // Log per-iteration timing breakdown at trace level when rate is logged
        if (iterRateLogCount == 0) {
            const int64_t msgUs = postMsgUs - iterNowUs;
            const int64_t healthUs = postHealthUs - postMsgUs;
            const int64_t configUs = preWaitUs - postHealthUs;
            LogDebug("[ControllerDiag] iter=%llu breakdown: msg=%lld health=%lld config=%lld",
                     (unsigned long long)iterCount, (long long)msgUs, (long long)healthUs, (long long)configUs);
            LogDebug("[ControllerDiag] iter=%llu waitMs=%lu msgCount=%d (timer=%d other=%d hk=%d)",
                     (unsigned long long)iterCount, waitMs, msgCount, msgTimers, msgOthers, msgHotkeys);
            if (waitMs == 0) {
                LogDebug("[ControllerDiag] iter=%llu WAITMS_ZERO cfgElapsed=%lu", (unsigned long long)iterCount,
                         (unsigned long)(GetTickCount() - lastConfigCheck));
            }
        }

        MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, 0);
    }

    // Unregister hotkeys first
    UnregisterHotKey(NULL, HOTKEY_ID_RECORD);
    UnregisterHotKey(NULL, HOTKEY_ID_SCREENSHOT);

    // Keep tray icon alive during shutdown (animation already started by
    // right-click handler) Process messages during shutdown so animation
    // continues
    if (g_Tray) {
        g_Tray->StartShutdownAnimation();
    }

    // Shutdown pseudo-overlay before child processes
    if (g_PseudoOverlay) {
        g_PseudoOverlay->Shutdown();
        g_PseudoOverlay.reset();
    }

    ShutdownChildProcesses();

    // Complete exact unregistration before invalidating the console handler's
    // non-owning pointer. The destructor is idempotent through call_once.
    vulkanReg.Unregister();
    g_VulkanReg = nullptr;

    // Now remove tray icon after shutdown is complete
    if (tray) {
        tray->Remove();
    }
    g_Tray = nullptr;
    tray.reset();

    LogInfo("[Controller] Exiting");
    return 0;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (const std::optional<int> workerResult = TryRunProcessLoopbackWorkerHost()) {
        return *workerResult;
    }

    if (IsDumpHelperCommandLine(lpCmdLine)) {
        return RunDumpHelperFromCommandLine();
    }

    // Parse process mode from command line
    ProcessMode mode = ParseProcessMode(lpCmdLine);
    if (mode == ProcessMode::Controller && HasExactCommandLineArgument(L"--list-monitors")) {
        return ce::monitor_selection::WriteMonitorListToStandardOutput();
    }
    if (mode == ProcessMode::Controller) {
        // An external launcher may have requested process-start feedback. Clear
        // it before config, logging, registration, or child startup; internal
        // roles must never change the user's current cursor themselves.
        PrimeStartupCursor();
    }

    // Get paths
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exePath = buffer;
    std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
    g_ConfigPath = baseDir + "\\config.ini";

    // Load config early so directory and crash-handler setup can be gated on
    // the configured log_level. When log_level=none/off we skip everything to
    // guarantee the logs/ tree stays absent and no debug machinery runs.
    LoadConfig(g_ConfigPath, g_Config);

    std::string logsRootDir = baseDir + "\\logs";

    // Determine session directory: Controller generates a new timestamped folder,
    // child processes inherit the name from --session-dir= on the command line.
    if (mode == ProcessMode::Controller) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[32];
        snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        g_SessionDirName = ts;
        if (IsAnyLoggingEnabled(g_Config.logLevel)) {
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

    if (IsAnyLoggingEnabled(g_Config.logLevel)) {
        CreateDirectoryA(logsRootDir.c_str(), NULL);
        CreateDirectoryA(earlyLogsDir.c_str(), NULL);
        SetCrashDumpDirectory(earlyLogsDir);
        InstallCrashHandler();
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
                g_AutoRecordDelayMs = delayMs;
                g_AutoRecordDurationMs = durationMs;
                g_AutoRecordEnabled = true;
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
            g_DeferredLaunchPath = cmdLine.substr(valueStart);

            // Game launch will happen in ControllerMain AFTER child processes are
            // ready
            if (IsAnyLoggingEnabled(g_Config.logLevel)) {
                Log_Init(earlyLogsDir + "\\launcher.log", g_Config.logLevel);
                LogInfo("[Launcher] Deferred launch path: %s", g_DeferredLaunchPath.c_str());
            }

            // Continue as Controller
            mode = ProcessMode::Controller;
        }
    }

    // Setup logging with process-specific log file in session logs subfolder
    std::string logsDir = earlyLogsDir;
    const std::string processLogName = GetProcessLogFileName(mode, g_RecordingId, GetCurrentProcessId());
    std::string logPath = logsDir + "\\" + processLogName;
    g_Config.logFilePath = logPath;
    if (IsAnyLoggingEnabled(g_Config.logLevel)) {
        CreateDirectoryA(logsDir.c_str(), NULL);
        if (mode == ProcessMode::Controller)
            WriteSessionManifest(logsDir, g_Config, mode);
        else if (mode == ProcessMode::Media)
            WriteRecordingManifest(logsDir, g_Config, processLogName);
    }

    if (IsAnyLoggingEnabled(g_Config.logLevel)) {
        Log_Init(logPath, g_Config.logLevel);
        LogInfo("CaptureEngine Starting... Version: %s (Built: %s)", GetCaptureVersion(), GetBuildTimestamp());
        LogInfo("Process Mode: %s", mode == ProcessMode::Controller ? "Controller"
                                    : mode == ProcessMode::Inject   ? "Inject"
                                    : mode == ProcessMode::Media    ? "Media"
                                    : mode == ProcessMode::Limiter  ? "Limiter"
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
        CreateMutexA(0, FALSE, "Local\\CaptureEngine_Instance_Mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxA(NULL, "CaptureEngine is already running.", "Error", MB_ICONERROR);
            return 1;
        }
    }

    // Keep crash dumps under logs/. Config can only add a relative subfolder.
    std::string crashDir = logsDir;
    if (!g_Config.crashDumpDir.empty()) {
        std::filesystem::path configured = std::filesystem::path(g_Config.crashDumpDir).lexically_normal();
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
    if (IsAnyLoggingEnabled(g_Config.logLevel)) {
        SetCrashDumpDirectory(crashDir);
    }

    // Dispatch to appropriate process
    int result = 0;
    switch (mode) {
        case ProcessMode::Controller:
            result = ControllerMain(hInstance);
            break;
        case ProcessMode::Inject:
            result = InjectProcessMain(g_Config);
            break;
        case ProcessMode::Media:
            result = MediaProcessMain(g_Config);
            break;
        case ProcessMode::Limiter:
            result = LimiterProcessMain(g_Config);
            break;
        case ProcessMode::Logger:
            result = LoggerProcessMain(g_Config);
            break;
        case ProcessMode::Sensors:
            result = SensorProcessMain(g_Config);
            break;
    }

    // Cleanup
    Log_Shutdown();

    return result;
}
