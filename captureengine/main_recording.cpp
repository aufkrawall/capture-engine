#include "main_internal.h"

#include "../common/live_stream_config.h"

namespace {
bool IsControllerLiveStreamOutput() {
    return main_g_LiveStreamRecording;
}
}  // namespace

// Publish a recording-failure notification through the inject shared-memory
// channel that both the inject overlay and the pseudo overlay consume. The
// notification is transient (7 s, matching the finalization failure duration)
// and is only shown once the overlays are back in the idle recording state.
void PublishRecordingFailureOverlayNotification(const char* reason, bool streaming) {
    WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
        sharedMemory->runtimeState.notificationType.store(
            static_cast<uint32_t>(streaming ? OverlayNotificationType::StreamingFailed
                                            : OverlayNotificationType::RecordingFailed),
            std::memory_order_release);
        sharedMemory->runtimeState.notificationExpiry.store(GetTickCount64() + 7000ULL, std::memory_order_release);
    });
    LogInfo("[Controller] %s failure notification published (%s)", streaming ? "Stream" : "Recording",
            reason ? reason : "unspecified");
}

void CheckRecordingFailureState() {
    if (!main_g_Recording)
        return;

    uint32_t failureCode = static_cast<uint32_t>(RecordingFailureCode::None);
    if (!WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
            failureCode = sharedMemory->runtimeState.recordingFailureCode.load(std::memory_order_acquire);
        }) ||
        failureCode == static_cast<uint32_t>(RecordingFailureCode::None)) {
        return;
    }

    LogError("[Controller] Recording failed with code %u; stopping all recording state", failureCode);
    RequestRecordingStopAndReleaseMedia("recording failure", 1000);

    main_g_Recording = false;
    PublishRecordingStartIntent(RecordingStartIntent::Idle, "recording failure");
    PublishRecordingFailureOverlayNotification("recording failure", IsControllerLiveStreamOutput());
    WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
        // Consume the code so a stale value cannot fail a later recording start
        // before the media process resets it itself.
        sharedMemory->runtimeState.recordingFailureCode.store(
            static_cast<uint32_t>(RecordingFailureCode::None), std::memory_order_release);
    });
    if (main_g_AutoRecordEnabled) {
        LogError("[Controller] Auto-record disabled after a recording-integrity failure");
        main_g_AutoRecordEnabled = false;
        main_g_AutoRecordStartTime = 0;
    }
    if (main_g_Tray)
        main_g_Tray->SetRecordingState(false);
}

// Toggle recording - controller notifies inject which sets shared memory
// Media process polls shared memory flags - more reliable than pipe IPC
void ToggleRecording() {
    main_g_Recording = !main_g_Recording;

    if (main_g_Recording) {
        main_g_LiveStreamRecording = ce::live_stream::IsLiveStreamTarget(main_g_Config.video.outputDir);
        PrepareRecordingDiagnosticIdentity();
        WithInjectSharedMem([&](SharedMemoryLayout* shm) {
            shm->runtimeState.notificationExpiry.store(0, std::memory_order_release);
            shm->runtimeState.notificationType.store(static_cast<uint32_t>(OverlayNotificationType::None),
                                                     std::memory_order_release);
        });
        PublishRecordingStartIntent(RecordingStartIntent::Video, "record hotkey");
        LogInfo("[Controller] Starting recording...");

        if (!EnsureMediaProcessReady(10000)) {
            LogError("[Controller] Media process is not ready, cannot start recording");
            main_g_Recording = false;
            if (main_g_Tray)
                main_g_Tray->SetRecordingState(false);
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "media readiness failure");
            PublishRecordingFailureOverlayNotification("media readiness failure", IsControllerLiveStreamOutput());
            return;
        }

        bool limiterReady = true;
        if (main_g_Config.fpsLimiter.captureSyncEnabled) {
            MainThreadBlockTimer _blk("record-start limiter readiness wait");
            limiterReady = EnsureLimiterProcessReady(10000);
        }
        if (!limiterReady) {
            LogError("[Controller] Limiter process is not ready for capture-synced recording");
            main_g_Recording = false;
            if (main_g_Tray)
                main_g_Tray->SetRecordingState(false);
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "limiter readiness failure");
            PublishRecordingFailureOverlayNotification("limiter readiness failure", IsControllerLiveStreamOutput());
            return;
        }

        // Notify inject process - it sets shared memory flags and media polls them
        if (main_g_InjectClient && main_g_InjectClient->IsConnected()) {
            ProcessResponse resp;
            if (main_g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                LogInfo("[Controller] Recording started");
            } else {
                LogError("[Controller] Failed to notify inject process - retrying once");
                // Retry once on failure
                if (main_g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                    LogInfo("[Controller] Recording started (retry)");
                } else {
                    LogError("[Controller] Recording start failed");
                    main_g_Recording = false;
                    PublishRecordingStartIntent(RecordingStartIntent::Idle, "inject start command failure");
                    PublishRecordingFailureOverlayNotification("inject start command failure",
                                                               IsControllerLiveStreamOutput());
                }
            }
        } else {
            LogError("[Controller] Inject process is not connected, cannot start recording");
            main_g_Recording = false;
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "inject unavailable");
            PublishRecordingFailureOverlayNotification("inject unavailable", IsControllerLiveStreamOutput());
        }
    } else {
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "record stop hotkey");
        WithInjectSharedMem([&](SharedMemoryLayout* shm) {
            shm->runtimeState.notificationType.store(
                static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing), std::memory_order_release);
            shm->runtimeState.notificationExpiry.store(GetTickCount64() + 60000ULL, std::memory_order_release);
        });
        if (main_g_PseudoOverlay) {
            main_g_PseudoOverlay->ShowRecordingFinalizingNotification();
        }
        LogInfo("[Controller] Stopping recording...");

        const bool stopAccepted = RequestRecordingStopAndReleaseMedia("record hotkey", 5000);
        if (stopAccepted) {
            LogInfo("[Controller] Recording stop accepted; media finalization continues asynchronously");
        } else {
            LogWarn("[Controller] Recording stop state cleared, but media finalization acceptance is unknown");
        }
    }

    if (main_g_Tray)
        main_g_Tray->SetRecordingState(main_g_Recording);
}

// Audio-only recording toggle (no video capture/encoding)
void ToggleAudioOnlyRecording() {
    main_g_Recording = !main_g_Recording;

    if (main_g_Recording) {
        main_g_LiveStreamRecording = false;
        PrepareRecordingDiagnosticIdentity();
        WithInjectSharedMem([&](SharedMemoryLayout* shm) {
            shm->runtimeState.notificationExpiry.store(0, std::memory_order_release);
            shm->runtimeState.notificationType.store(static_cast<uint32_t>(OverlayNotificationType::None),
                                                     std::memory_order_release);
        });
        const bool audioOnlySet =
            PublishRecordingStartIntent(RecordingStartIntent::AudioOnly, "audio-only hotkey");
        LogInfo("[Controller] Starting audio-only recording...");

        if (!EnsureMediaProcessReady(10000)) {
            LogError("[Controller] Media process is not ready, cannot start audio-only recording");
            main_g_Recording = false;
            if (main_g_Tray)
                main_g_Tray->SetRecordingState(false);
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "audio-only media readiness failure");
            PublishRecordingFailureOverlayNotification("audio-only media readiness failure");
            return;
        }

        if (!audioOnlySet) {
            LogWarn("[Controller] No inject shared memory found for audio-only flag - media will use separate IPC");
            PublishRecordingStartIntent(RecordingStartIntent::AudioOnly, "audio-only shared-state retry");
        }

        bool startCommandDelivered = false;

        // Notify inject process - it sets cmdStartRecording in shared memory.
        // The audio-only flag and pending intent were already published above.
        if (main_g_InjectClient && main_g_InjectClient->IsConnected()) {
            ProcessResponse resp;
            if (main_g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                startCommandDelivered = true;
                LogInfo("[Controller] Audio-only recording request delivered through inject");
            } else {
                LogWarn("[Controller] Failed to notify inject for audio-only recording; trying media directly");
            }
        }

        // Also notify media directly. This is authoritative when inject is unavailable
        // and harmless when the shared-memory request won the race first.
        if (main_g_MediaClient && main_g_MediaClient->IsConnected()) {
            ProcessResponse resp = ProcessResponse::Error;
            if (main_g_MediaClient->SendCommand(ProcessCommand::StartRecording, "audio_only", &resp, 5000) &&
                resp != ProcessResponse::Error) {
                startCommandDelivered = true;
                LogInfo("[Controller] Audio-only recording request accepted by media");
            }
        }
        if (!startCommandDelivered) {
            LogError("[Controller] Audio-only recording start failed");
            main_g_Recording = false;
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "audio-only start command failure");
            PublishRecordingFailureOverlayNotification("audio-only start command failure");
        }
    } else {
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "audio-only stop hotkey");
        WithInjectSharedMem([&](SharedMemoryLayout* shm) {
            shm->runtimeState.notificationType.store(
                static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing), std::memory_order_release);
            shm->runtimeState.notificationExpiry.store(GetTickCount64() + 60000ULL, std::memory_order_release);
        });
        if (main_g_PseudoOverlay) {
            main_g_PseudoOverlay->ShowRecordingFinalizingNotification();
        }
        LogInfo("[Controller] Stopping audio-only recording...");

        const bool stopAccepted = RequestRecordingStopAndReleaseMedia("audio-only hotkey", 5000);
        if (stopAccepted) {
            LogInfo("[Controller] Audio-only recording stop accepted; media finalization continues asynchronously");
        } else {
            LogWarn("[Controller] Audio-only stop state cleared, but media finalization acceptance is unknown");
        }
    }

    if (main_g_Tray)
        main_g_Tray->SetRecordingState(main_g_Recording);
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

// Shutdown all child processes gracefully
void ShutdownChildProcesses() {
    LogInfo("[Controller] Shutting down child processes...");
    PublishRecordingStartIntent(RecordingStartIntent::Idle, "controller shutdown");

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

    if (main_g_hLimiterProcess) {
        handles[handleCount] = main_g_hLimiterProcess;
        handleNames[handleCount++] = "Limiter";
    }
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
            if (main_g_hLimiterProcess)
                TerminateProcess(main_g_hLimiterProcess, 1);
            if (main_g_hMediaProcess)
                TerminateProcess(main_g_hMediaProcess, 1);
            if (main_g_hInjectProcess)
                TerminateProcess(main_g_hInjectProcess, 1);
            if (main_g_hLoggerProcess)
                TerminateProcess(main_g_hLoggerProcess, 1);
            if (main_g_hSensorProcess)
                TerminateProcess(main_g_hSensorProcess, 1);
        } else {
            LogInfo("[Controller] All child processes exited cleanly");
        }
    }

    // Cleanup handles
    if (main_g_hLimiterProcess)
        CloseHandle(main_g_hLimiterProcess);
    if (main_g_hMediaProcess)
        CloseHandle(main_g_hMediaProcess);
    if (main_g_hInjectProcess)
        CloseHandle(main_g_hInjectProcess);
    if (main_g_hLoggerProcess)
        CloseHandle(main_g_hLoggerProcess);
    if (main_g_hSensorProcess)
        CloseHandle(main_g_hSensorProcess);

    main_g_hLimiterProcess = NULL;
    main_g_hMediaProcess = NULL;
    main_g_hInjectProcess = NULL;
    main_g_hLoggerProcess = NULL;
    main_g_hSensorProcess = NULL;
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

    RecordingStartIntent recordingStartIntent = main_g_RecordingStartIntent.load(std::memory_order_acquire);
    if (main_g_Recording && recordingStartIntent != RecordingStartIntent::Idle) {
        WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
            if (sharedMemory->runtimeState.isRecording.load(std::memory_order_acquire)) {
                recordingStartIntent = RecordingStartIntent::Idle;
                main_g_RecordingStartIntent.store(RecordingStartIntent::Idle, std::memory_order_release);
            }
        });
    }

    const bool recordingStartPending = main_g_Recording && recordingStartIntent != RecordingStartIntent::Idle;
    const bool mediaUnavailable = recordingStartPending &&
                                  (!main_g_hMediaProcess || !IsProcessRunning(main_g_hMediaProcess) || !main_g_MediaClient ||
                                   !main_g_MediaClient->IsConnected());
    const bool injectUnavailable = recordingStartPending && recordingStartIntent == RecordingStartIntent::Video &&
                                   (!main_g_hInjectProcess || !IsProcessRunning(main_g_hInjectProcess) || !main_g_InjectClient ||
                                    !main_g_InjectClient->IsConnected());
    const bool limiterUnavailable =
        recordingStartPending && recordingStartIntent == RecordingStartIntent::Video &&
        main_g_Config.fpsLimiter.captureSyncEnabled &&
        (!main_g_hLimiterProcess || !IsProcessRunning(main_g_hLimiterProcess) || !main_g_LimiterClient ||
         !main_g_LimiterClient->IsConnected());
    if (mediaUnavailable || injectUnavailable || limiterUnavailable) {
        const char* failedChild = mediaUnavailable ? "media" : (injectUnavailable ? "inject" : "limiter");
        LogError("[Controller] Required %s process/channel exited before recording became live; cancelling start intent",
                 failedChild);
        RequestRecordingStopAndReleaseMedia("required child exited before recording live", 1000);
        main_g_Recording = false;
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "required child exited before recording live");
        PublishRecordingFailureOverlayNotification(
            "required child exited before recording live",
            recordingStartIntent == RecordingStartIntent::Video && IsControllerLiveStreamOutput());
        if (main_g_Tray) {
            main_g_Tray->SetRecordingState(false);
        }
    }

    // A live recording whose media process died cannot finalize through the
    // normal stop path: no failure code and no completion notification will
    // ever arrive from it. Report the failed capture in both overlays and clear
    // the hook-facing recording state so the REC indicator cannot stay stuck
    // after the process is gone; the recovery below still respawns an idle
    // media process for the next recording.
    bool recordingLive = false;
    bool recordingLiveAudioOnly = false;
    WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
        recordingLive = sharedMemory->runtimeState.isRecording.load(std::memory_order_acquire);
        recordingLiveAudioOnly = sharedMemory->runtimeState.audioOnly.load(std::memory_order_acquire);
    });
    const bool mediaGoneWhileLive =
        main_g_Recording && recordingLive && (!main_g_hMediaProcess || !IsProcessRunning(main_g_hMediaProcess));
    if (mediaGoneWhileLive) {
        LogError("[Controller] Media process exited while recording was live; recording failed");
        main_g_Recording = false;
        WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
            sharedMemory->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
            sharedMemory->runtimeState.captureRequested.store(false, std::memory_order_release);
            sharedMemory->runtimeState.isRecording.store(false, std::memory_order_release);
            sharedMemory->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        });
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "media process exited while recording live");
        PublishRecordingFailureOverlayNotification("media process exited while recording live",
                                                   !recordingLiveAudioOnly && IsControllerLiveStreamOutput());
        if (main_g_AutoRecordEnabled) {
            LogError("[Controller] Auto-record disabled after the media process exited during recording");
            main_g_AutoRecordEnabled = false;
            main_g_AutoRecordStartTime = 0;
        }
        if (main_g_Tray) {
            main_g_Tray->SetRecordingState(false);
        }
    }

    auto recoverProcess = [](ProcessMode mode, HANDLE& process, ProcessIPCClient* client, const char* name,
                             bool expected, bool& recoveryFailureReported) {
        if (!expected)
            return;
        if (process && IsProcessRunning(process) && client && client->IsConnected()) {
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
    static bool limiterRecoveryFailure = false;
    recoverProcess(ProcessMode::Inject, main_g_hInjectProcess, main_g_InjectClient.get(), "inject", true, injectRecoveryFailure);
    recoverProcess(ProcessMode::Media, main_g_hMediaProcess, main_g_MediaClient.get(), "media", main_g_hMediaProcess != nullptr,
                   mediaRecoveryFailure);
    recoverProcess(ProcessMode::Limiter, main_g_hLimiterProcess, main_g_LimiterClient.get(), "limiter",
                   main_g_hLimiterProcess != nullptr, limiterRecoveryFailure);
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

    int64_t limiterSpawnUs = 0;
    if (ShouldStartLimiterProcessAtStartup(main_g_Config)) {
        const int64_t limiterSpawnStartUs = Log_GetQpcUs();
        main_g_hLimiterProcess = SpawnChildProcess(ProcessMode::Limiter, main_g_ConfigPath.c_str(), main_g_LimiterClient.get());
        limiterSpawnUs = Log_GetQpcUs() - limiterSpawnStartUs;
        if (!main_g_hLimiterProcess) {
            LogError("[Controller] Failed to spawn limiter process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring limiter process startup until a limiter is enabled");
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

    LogInfo("[Controller] Waiting for child processes to connect...");
    const int64_t ipcConnectStartUs = Log_GetQpcUs();
    if (!ConnectToChildProcesses(10000)) {
        LogError("[Controller] Failed to connect to all child processes");
        return false;
    }
    const int64_t ipcConnectUs = Log_GetQpcUs() - ipcConnectStartUs;
    LogInfo("[Controller] All child processes connected");

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

    // RegisterHotKey stops being delivered to anyone while a foreground
    // application registers its raw-input keyboard with RIDEV_NOHOTKEYS, so the
    // same hotkeys are also recognized on a low-level keyboard hook. This runs
    // on the controller thread, which is the thread RegisterHotKey posts to and
    // therefore the thread the hook has to post to as well.
    PublishHotkeyBindings(main_g_Config, main_g_HotkeyOwnership);
    StartHotkeyInputHook(GetCurrentThreadId());
    const int64_t hotkeyUs = Log_GetQpcUs() - hotkeyStartUs;

    SyncPseudoOverlayConfiguration("startup");

    LogInfo("[Controller] Ready. Press hotkey to start recording.");
    PrimeStartupCursor();
    LogInfo(
        "[StartupPerf] Controller startup: VulkanRegistration=%.3f ms, SpawnInject=%.3f ms, "
        "SpawnMedia=%.3f ms, SpawnLimiter=%.3f ms, SpawnAux=%.3f ms, IPCConnect=%.3f ms, TrayCreate=%.3f ms, "
        "RegisterHotkeys=%.3f ms, TotalToReady=%.3f ms",
        QpcDeltaToMs(main_g_ControllerStartupTiming.vulkanRegUs), QpcDeltaToMs(injectSpawnUs), QpcDeltaToMs(mediaSpawnUs),
        QpcDeltaToMs(limiterSpawnUs), QpcDeltaToMs(auxSpawnUs), QpcDeltaToMs(ipcConnectUs),
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
