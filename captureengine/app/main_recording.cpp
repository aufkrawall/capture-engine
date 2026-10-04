#include "main_internal.h"
#include "libcaptureengine_controller.h"

#include "common/ipc/inject_control_channel.h"

void PublishRecordingFailureOverlayNotification(const char* reason, bool streaming) {
    ce::ipc::InjectControlChannel(main_g_hInjectProcess).PublishNotification(
        streaming ? OverlayNotificationType::StreamingFailed : OverlayNotificationType::RecordingFailed,
        GetTickCount64() + 7000ULL);
    LogError("[ControllerSession] Recording failure (%s, streaming=%d request=%llu)", reason, streaming ? 1 : 0,
             static_cast<unsigned long long>(ControllerRecordingSnapshot().request));
}

void CheckRecordingFailureState() { ReconcileControllerRecording(false); }

void ToggleRecording() {
    if (ControllerRecordingSnapshot().requested)
        StopControllerRecording("record hotkey");
    else
        StartControllerRecording(RecordingStartIntent::Video, "record hotkey");
}

void ToggleAudioOnlyRecording() {
    if (ControllerRecordingSnapshot().requested)
        StopControllerRecording("audio-only hotkey");
    else
        StartControllerRecording(RecordingStartIntent::AudioOnly, "audio-only hotkey");
}

// Toggle the injected in-game overlay on/off at runtime. The inject process owns
// the shared-memory overlay config, so the controller only forwards the intent;
// this keeps the overlay-config seqlock single-writer.
void ToggleOverlay() {
    if (!main_g_InjectClient || !main_g_InjectClient->IsConnected()) {
        LogWarn("[Controller] Overlay toggle hotkey pressed, but no inject process is connected");
        return;
    }

    MainThreadBlockTimer _blk("overlay toggle IPC");
    ProcessResponse response = ProcessResponse::Error;
    if (!main_g_InjectClient->SendCommand(ProcessCommand::ToggleOverlay, nullptr, &response) ||
        response == ProcessResponse::Error) {
        LogError("[Controller] Inject process did not accept the overlay toggle");
        return;
    }
    LogInfo("[Controller] Overlay toggle hotkey handled");
}

void ToggleBenchmark() {
    if (!main_g_InjectClient || !main_g_InjectClient->IsConnected()) {
        LogWarn("[Controller] Benchmark hotkey pressed, but no inject process is connected");
        return;
    }

    MainThreadBlockTimer _blk("benchmark toggle IPC");
    ProcessResponse response = ProcessResponse::Error;
    if (!main_g_InjectClient->SendCommand(ProcessCommand::ToggleBenchmark, nullptr, &response) ||
        response == ProcessResponse::Error) {
        LogError("[Controller] Inject process did not accept the benchmark toggle");
        return;
    }
    LogInfo("[Controller] Benchmark toggle hotkey handled");
}

// Shutdown all child processes gracefully
void ShutdownChildProcesses() {
    LogInfo("[Controller] Shutting down child processes...");
    ShutdownControllerRecording();

    // Signal Logger and Sensor processes to exit via named event
    wchar_t shutdownEventName[64];
    GenerateShutdownEventName(shutdownEventName, 64, GetCurrentProcessId());
    HANDLE hShutdownEvent = CreateEventW(NULL, TRUE, FALSE, shutdownEventName);
    if (hShutdownEvent) {
        SetEvent(hShutdownEvent);
        CloseHandle(hShutdownEvent);
    }

    // Send shutdown commands
    SendCommandToAll(ProcessCommand::Shutdown);

    // Wait for processes to exit
    HANDLE handles[5];  // Increased size for Logger and Sensor
    const char* handleNames[5] = {};
    int handleCount = 0;

    if (main_g_hMediaProcess) {
        handles[handleCount] = main_g_hMediaProcess;
        handleNames[handleCount++] = "Media";
    }
    if (main_g_hInjectProcess) {
        handles[handleCount] = main_g_hInjectProcess;
        handleNames[handleCount++] = "Inject";
    }
    if (main_g_hLoggerProcess) {
        handles[handleCount] = main_g_hLoggerProcess;
        handleNames[handleCount++] = "Logger";
    }
    if (main_g_hSensorProcess) {
        handles[handleCount] = main_g_hSensorProcess;
        handleNames[handleCount++] = "Sensor";
    }

    if (handleCount > 0) {
        // Use MsgWaitForMultipleObjects to keep processing messages (for tray
        // animation)
        DWORD startTime = GetTickCount();
        // INCREASED TIMEOUT: Media process needs more time to flush video/audio
        // data especially for high-resolution recordings (4K 120fps)
        DWORD timeout = 10000;  // 10 seconds (was 5)
        bool allExited = false;

        while (!allExited && (GetTickCount() - startTime) < timeout) {
            DWORD remaining = timeout - (GetTickCount() - startTime);
            DWORD waitTime = (remaining < 100) ? remaining : 100;

            // bWaitAll MUST be FALSE to process messages while waiting
            DWORD result = MsgWaitForMultipleObjects(handleCount, handles, FALSE, waitTime, QS_ALLINPUT);

            if (result == WAIT_OBJECT_0 + handleCount) {
                // Messages available - process them to keep tray animation running
                MSG msg;
                while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            } else if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handleCount) {
                // At least one process exited, re-evaluate all processes
                bool foundActive = false;
                for (int i = 0; i < handleCount; i++) {
                    DWORD exitCode;
                    if (GetExitCodeProcess(handles[i], &exitCode) && exitCode == STILL_ACTIVE) {
                        foundActive = true;
                        break;
                    }
                }
                if (!foundActive)
                    allExited = true;
            } else {
                // Timeout or other error
            }
        }

        if (!allExited) {
            // Log which specific processes didn't exit cleanly for debugging
            LogInfo(
                "[Controller] Some processes didn't exit cleanly within timeout, "
                "terminating...");
            for (int i = 0; i < handleCount; i++) {
                DWORD exitCode;
                if (GetExitCodeProcess(handles[i], &exitCode) && exitCode == STILL_ACTIVE) {
                    LogInfo(
                        "[Controller] %s process did not exit cleanly, forcing "
                        "termination",
                        handleNames[i]);
                }
            }
            if (main_g_hMediaProcess)
                TerminateProcess(main_g_hMediaProcess, 1);
            if (main_g_hInjectProcess)
                TerminateProcess(main_g_hInjectProcess, 1);
            if (main_g_hLoggerProcess)
                TerminateProcess(main_g_hLoggerProcess, 1);
            if (main_g_hSensorProcess)
                TerminateProcess(main_g_hSensorProcess, 1);
            // TerminateProcess is asynchronous. Do not release ownership while
            // a child still maps product files or owns an out-of-process worker.
            for (int i = 0; i < handleCount; ++i) {
                if (WaitForSingleObject(handles[i], INFINITE) != WAIT_OBJECT_0)
                    LogError("[Controller] Cannot confirm %s process exit (error=%lu)", handleNames[i], GetLastError());
            }
        } else {
            LogInfo("[Controller] All child processes exited cleanly");
        }
    }

    // Cleanup handles
    if (main_g_hMediaProcess)
        CloseHandle(main_g_hMediaProcess);
    if (main_g_hInjectProcess)
        CloseHandle(main_g_hInjectProcess);
    if (main_g_hLoggerProcess)
        CloseHandle(main_g_hLoggerProcess);
    if (main_g_hSensorProcess)
        CloseHandle(main_g_hSensorProcess);

    main_g_hMediaProcess = NULL;
    main_g_hInjectProcess = NULL;
    main_g_hLoggerProcess = NULL;
    main_g_hSensorProcess = NULL;

    // Every child that could have inherited it is gone; the session A/V latency channel has no
    // further readers.
    ce::av_sync::ReleaseSessionLatencyChannel();
}

// Monitor authenticated children and replace a process only after its broken
// channel has caused the old instance to exit. Media and limiter are recovered
// only while the controller still owns a handle for an expected live instance;
// their normal deferred/off states deliberately keep a null handle.
void CheckChildProcessHealth() {
    static DWORD lastCheck = 0;
    if (GetTickCount() - lastCheck < 1000)
        return;  // Check once per second
    lastCheck = GetTickCount();

    ReconcileControllerRecording(true);

    auto recoverProcess = [](ProcessMode mode, HANDLE& process, ProcessIPCClient* client, const char* name,
                             bool expected, bool& recoveryFailureReported) {
        if (!expected)
            return;
        if (process && IsProcessRunning(process) && (!client || client->IsConnected())) {
            recoveryFailureReported = false;
            return;
        }

        if (EnsureChildProcessConnected(mode, process, client, 2000, name)) {
            LogInfo("[Controller] Recovered %s after process exit or authenticated-channel failure", name);
            recoveryFailureReported = false;
            return;
        }
        if (!recoveryFailureReported) {
            LogError("[Controller] Could not yet recover %s after process exit or IPC failure", name);
            recoveryFailureReported = true;
        }
    };

    static bool injectRecoveryFailure = false;
    static bool mediaRecoveryFailure = false;
    static bool sensorRecoveryFailure = false;
    recoverProcess(ProcessMode::Inject, main_g_hInjectProcess, main_g_InjectClient.get(), "inject", true, injectRecoveryFailure);
    recoverProcess(ProcessMode::Media, main_g_hMediaProcess, main_g_MediaClient.get(), "media", main_g_hMediaProcess != nullptr,
                   mediaRecoveryFailure);
    recoverProcess(ProcessMode::Sensors, main_g_hSensorProcess, nullptr, "sensor",
                   ShouldStartSensorProcess(main_g_Config), sensorRecoveryFailure);
}

bool CompleteControllerStartup() {
    if (main_g_ControllerStartupTiming.complete) {
        return true;
    }

    LogInfo("[Controller] Completing deferred startup...");
    LogInfo("[Controller] Spawning child processes...");

    const int64_t injectSpawnStartUs = Log_GetQpcUs();
    main_g_hInjectProcess = SpawnChildProcess(ProcessMode::Inject, main_g_ConfigPath.c_str(), main_g_InjectClient.get());
    const int64_t injectSpawnUs = Log_GetQpcUs() - injectSpawnStartUs;
    if (!main_g_hInjectProcess) {
        LogError("[Controller] Failed to spawn inject process");
        return false;
    }

    int64_t mediaSpawnUs = 0;
    if (ShouldStartMediaProcessAtStartup()) {
        PrepareRecordingDiagnosticIdentity();
        const int64_t mediaSpawnStartUs = Log_GetQpcUs();
        main_g_hMediaProcess = SpawnChildProcess(ProcessMode::Media, main_g_ConfigPath.c_str(), main_g_MediaClient.get());
        mediaSpawnUs = Log_GetQpcUs() - mediaSpawnStartUs;
        if (!main_g_hMediaProcess) {
            LogError("[Controller] Failed to spawn media process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring media process startup until recording begins");
    }

    const int64_t auxSpawnStartUs = Log_GetQpcUs();
    if (ShouldStartLoggerProcess(main_g_Config)) {
        main_g_hLoggerProcess = SpawnChildProcess(ProcessMode::Logger, main_g_ConfigPath.c_str());
        if (!main_g_hLoggerProcess) {
            LogError("[Controller] Failed to spawn logger process");
        }
    }
    if (ShouldStartSensorProcess(main_g_Config)) {
        main_g_hSensorProcess = SpawnChildProcess(ProcessMode::Sensors, main_g_ConfigPath.c_str());
        if (!main_g_hSensorProcess) {
            LogError("[Controller] Failed to spawn sensor process");
        }
    }
    const int64_t auxSpawnUs = Log_GetQpcUs() - auxSpawnStartUs;
    PumpStartupMessages();

    LogInfo("[Controller] Waiting for child processes to connect...");
    const int64_t ipcConnectStartUs = Log_GetQpcUs();
    if (!ConnectToChildProcesses(10000)) {
        LogError("[Controller] Failed to connect to all child processes");
        return false;
    }
    const int64_t ipcConnectUs = Log_GetQpcUs() - ipcConnectStartUs;
    LogInfo("[Controller] All child processes connected");
    PumpStartupMessages();

    if (!main_g_DeferredLaunchPath.empty()) {
        LogInfo("[Controller] Launching deferred game: %s", main_g_DeferredLaunchPath.c_str());
        LaunchGameSuspended(main_g_DeferredLaunchPath);
    }

    LogInfo("[Controller] Registering hotkeys...");
    const int64_t hotkeyStartUs = Log_GetQpcUs();

    main_g_HotkeyOwnership.record =
        RegisterConfiguredHotkey(HOTKEY_ID_RECORD, main_g_Config.hotkeyStartStop, "recording");
    main_g_HotkeyOwnership.screenshot =
        RegisterConfiguredHotkey(HOTKEY_ID_SCREENSHOT, main_g_Config.hotkeyScreenshot, "screenshot");
    main_g_HotkeyOwnership.audioOnly =
        RegisterConfiguredHotkey(HOTKEY_ID_AUDIO_ONLY, main_g_Config.hotkeyAudioOnly, "audio-only");
    main_g_HotkeyOwnership.toggleOverlay =
        RegisterConfiguredHotkey(HOTKEY_ID_TOGGLE_OVERLAY, main_g_Config.hotkeyToggleOverlay, "overlay toggle");
    main_g_HotkeyOwnership.benchmark =
        RegisterConfiguredHotkey(HOTKEY_ID_BENCHMARK, main_g_Config.hotkeyBenchmark, "benchmark");

    // RegisterHotKey stops being delivered to anyone while a foreground
    // application registers its raw-input keyboard with RIDEV_NOHOTKEYS, so the
    // same hotkeys are also recognized on a low-level keyboard hook. This runs
    // on the controller thread, which is the thread RegisterHotKey posts to and
    // therefore the thread the hook has to post to as well.
    PublishHotkeyBindings(main_g_Config, main_g_HotkeyOwnership);
    StartHotkeyInputHook(GetCurrentThreadId());
    const int64_t hotkeyUs = Log_GetQpcUs() - hotkeyStartUs;

    SyncPseudoOverlayConfiguration("startup");
    PumpStartupMessages();

    LogInfo("[Controller] Ready. Press hotkey to start recording.");
    PrimeStartupCursor();
    LogInfo(
        "[StartupPerf] Controller startup: VulkanRegistration=%.3f ms, SpawnInject=%.3f ms, "
        "SpawnMedia=%.3f ms, SpawnAux=%.3f ms, IPCConnect=%.3f ms, TrayCreate=%.3f ms, "
        "RegisterHotkeys=%.3f ms, TotalToReady=%.3f ms",
        QpcDeltaToMs(main_g_ControllerStartupTiming.vulkanRegUs), QpcDeltaToMs(injectSpawnUs), QpcDeltaToMs(mediaSpawnUs),
        QpcDeltaToMs(auxSpawnUs), QpcDeltaToMs(ipcConnectUs),
        QpcDeltaToMs(main_g_ControllerStartupTiming.trayCreateUs), QpcDeltaToMs(hotkeyUs),
        QpcDeltaToMs(Log_GetQpcUs() - main_g_ControllerStartupTiming.controllerStartUs));

    if (main_g_AutoRecordEnabled) {
        LogInfo("[Controller] Auto-record enabled: delay=%lums, duration=%lums", main_g_AutoRecordDelayMs,
                main_g_AutoRecordDurationMs);
        main_g_AutoRecordStartTime = GetTickCount();
    }

    main_g_ControllerStartupTiming.complete = true;
    return true;
}
