         lowerName.find("_vk") != std::string::npos || lowerName.find("game") != std::string::npos ||
         lowerName.find("_test") != std::string::npos || lowerName.find("test.exe") != std::string::npos);
    bool looksLikeLauncher = !looksLikeGame;

    if (looksLikeLauncher) {
        // This looks like a launcher - start it NORMALLY, no injection
        // We'll rely on WMI to catch the actual game
        LogInfo("[Launcher] Detected launcher (not game): %s - Starting normally",
                launchCommand.rawCommandLine.c_str());

        if (CreateProcessA(cleanPath.c_str(), mutableCommandLine, NULL, NULL, FALSE, 0, NULL, workingDir, &si, &pi)) {
            LogInfo("[Launcher] Launcher started (PID: %lu). WMI will catch the game.", pi.dwProcessId);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to start launcher: %lu", GetLastError());
        }
    } else {
        // This looks like the actual game - use suspended injection
        LogInfo("[Launcher] Detected game: %s - Launching Suspended", launchCommand.rawCommandLine.c_str());

        if (CreateProcessA(cleanPath.c_str(), mutableCommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, workingDir,
                           &si, &pi)) {
            LogInfo(
                "[Launcher] Process Created (PID: %lu). Attempting early APC "
                "injection...",
                pi.dwProcessId);

            auto injector = std::make_shared<InjectionManager>(g_Config);

            // Determine DLL path based on target architecture
            BOOL isWow64 = FALSE;
            HANDLE hCheckProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pi.dwProcessId);
            if (hCheckProcess) {
                IsWow64Process(hCheckProcess, &isWow64);
                CloseHandle(hCheckProcess);
            }

            std::string hookDllPath;
            char buffer[MAX_PATH];
            GetModuleFileNameA(NULL, buffer, MAX_PATH);
            std::string baseDir = std::string(buffer).substr(0, std::string(buffer).find_last_of("\\/"));
            hookDllPath = isWow64 ? (baseDir + "\\capture_hook_x86.dll") : (baseDir + "\\capture_hook_x64.dll");

            // Try early APC injection first (runs before import resolution)
            bool injected = injector->InjectEarly(pi.dwProcessId, hookDllPath, pi.hThread);

            if (injected) {
                LogInfo("[Launcher] Early APC injection successful. Resuming thread.");
                ResumeThread(pi.hThread);
            } else {
                LogInfo(
                    "[Launcher] APC injection failed, falling back to "
                    "CreateRemoteThread...");
                ResumeThread(pi.hThread);

                // Fallback to traditional injection
                Sleep(100);  // Give process a moment to initialize
                if (injector->Inject(pi.dwProcessId, launchCommand.fileName)) {
                    LogInfo("[Launcher] Fallback injection successful.");
                } else {
                    LogError(
                        "[Launcher] Fallback injection FAILED. Game running without "
                        "hooks.");
                }
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to CreateProcess: %lu", GetLastError());
        }
    }
}

// Spawn authenticates IPC synchronously; there is no reconnect-by-name phase.
bool ConnectToChildProcesses(DWORD) {
    return (!g_hInjectProcess || g_InjectClient->IsConnected()) && (!g_hMediaProcess || g_MediaClient->IsConnected()) &&
           (!g_hLimiterProcess || g_LimiterClient->IsConnected());
}

// Send command to all child processes
void SendCommandToAll(ProcessCommand cmd) {
    if (g_InjectClient && g_InjectClient->IsConnected()) {
        g_InjectClient->SendCommand(cmd);
    }
    if (g_MediaClient && g_MediaClient->IsConnected()) {
        g_MediaClient->SendCommand(cmd);
    }
    if (g_LimiterClient && g_LimiterClient->IsConnected()) {
        g_LimiterClient->SendCommand(cmd);
    }
}

// Helper: open inject's shared memory and run a callback with a writable view.
// Returns true if the shared memory was opened and the callback executed.
static bool WithInjectSharedMem(std::function<void(SharedMemoryLayout*)> fn) {
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (!hDisc)
        return false;
    DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
    if (!ValidateDiscoveryInfo(pDisc)) {
        if (pDisc)
            UnmapViewOfFile(pDisc);
        CloseHandle(hDisc);
        return false;
    }
    uint32_t injPid = pDisc->GetInjectPid();
    UnmapViewOfFile(pDisc);
    CloseHandle(hDisc);
    if (injPid == 0)
        return false;

    wchar_t shmName[64];
    GenerateSharedMemName(shmName, 64, injPid);
    HANDLE hShm = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, shmName);
    if (!hShm)
        return false;
    auto* pShm =
        (SharedMemoryLayout*)MapViewOfFile(hShm, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout));
    if (!pShm) {
        CloseHandle(hShm);
        return false;
    }
    if (!ValidateSharedMemory(pShm)) {
        LogError("[Controller] Rejected inject shared memory with incompatible ABI (version=%u size=%u abi=0x%08X)",
                 pShm->GetVersion(), pShm->structSize.load(std::memory_order_acquire),
                 pShm->abiSignature.load(std::memory_order_acquire));
        UnmapViewOfFile(pShm);
        CloseHandle(hShm);
        return false;
    }
    fn(pShm);
    UnmapViewOfFile(pShm);
    CloseHandle(hShm);
    return true;
}

static bool PublishRecordingStartIntent(RecordingStartIntent intent, const char* reason) {
    g_RecordingStartIntent.store(intent, std::memory_order_release);
    const bool published = WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
        sharedMemory->runtimeState.SetRecordingStartIntent(intent);
        if (intent != RecordingStartIntent::AudioOnly) {
            sharedMemory->runtimeState.audioOnly.store(false, std::memory_order_release);
        } else {
            sharedMemory->runtimeState.audioOnly.store(true, std::memory_order_release);
        }
    });
    if (g_PseudoOverlay) {
        g_PseudoOverlay->SetRecordingStartIntent(intent);
    }
    LogInfo("[Controller] Recording start intent=%s published=%d reason=%s",
            intent == RecordingStartIntent::Video       ? "video"
            : intent == RecordingStartIntent::AudioOnly ? "audio-only"
                                                        : "idle",
            published ? 1 : 0, reason ? reason : "unspecified");
    return published;
}

static bool RequestChildRecordingStop(ProcessIPCClient* client, const char* childName, const char* reason,
                                      DWORD timeoutMs) {
    if (!client || !client->IsConnected())
        return false;

    ProcessResponse response = ProcessResponse::Error;
    if (!client->SendCommand(ProcessCommand::StopRecording, nullptr, &response, timeoutMs) ||
        response == ProcessResponse::Error) {
        LogWarn("[Controller] %s did not accept the recording stop (%s)", childName,
                reason ? reason : "unspecified");
        return false;
    }

    LogInfo("[Controller] %s accepted the recording stop (%s)", childName, reason ? reason : "unspecified");
    return true;
}

static bool RequestRecordingStopAndReleaseMedia(const char* reason, DWORD timeoutMs) {
    // Ask media first. It acknowledges before finalization, so controller UI work
    // does not wait for trailer writing or the post-mux probe. Media clears the
    // hook-facing shared state before acknowledging. The inject command is only a
    // fallback when the private media channel cannot accept the request.
    const bool mediaAccepted = RequestChildRecordingStop(g_MediaClient.get(), "Media", reason, timeoutMs);
    const bool stopAccepted =
        mediaAccepted || RequestChildRecordingStop(g_InjectClient.get(), "Inject fallback", reason, timeoutMs);
    if (!stopAccepted) {
        LogWarn("[Controller] No recording child accepted the stop (%s); process teardown is the final fallback",
                reason ? reason : "unspecified");
    }

    // Media self-exits after finalization. Drop the controller's reference now so
    // the next recording creates a fresh authenticated child.
    if (g_MediaClient)
        g_MediaClient->Disconnect();
    CloseProcessHandle(g_hMediaProcess);
    return stopAccepted;
}

void CheckRecordingFailureState() {
    if (!g_Recording)
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

    g_Recording = false;
    PublishRecordingStartIntent(RecordingStartIntent::Idle, "recording failure");
    if (g_AutoRecordEnabled) {
        LogError("[Controller] Auto-record disabled after a recording-integrity failure");
        g_AutoRecordEnabled = false;
        g_AutoRecordStartTime = 0;
    }
    if (g_Tray)
        g_Tray->SetRecordingState(false);
}

// Toggle recording - controller notifies inject which sets shared memory
// Media process polls shared memory flags - more reliable than pipe IPC
void ToggleRecording() {
    g_Recording = !g_Recording;

    if (g_Recording) {
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
            g_Recording = false;
            if (g_Tray)
                g_Tray->SetRecordingState(false);
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "media readiness failure");
            return;
        }

        bool limiterReady = true;
        if (g_Config.fpsLimiter.captureSyncEnabled) {
            MainThreadBlockTimer _blk("record-start limiter readiness wait");
            limiterReady = EnsureLimiterProcessReady(10000);
        }
        if (!limiterReady) {
            LogError("[Controller] Limiter process is not ready for capture-synced recording");
            g_Recording = false;
            if (g_Tray)
                g_Tray->SetRecordingState(false);
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "limiter readiness failure");
            return;
        }

        // Notify inject process - it sets shared memory flags and media polls them
        if (g_InjectClient && g_InjectClient->IsConnected()) {
            ProcessResponse resp;
            if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                LogInfo("[Controller] Recording started");
            } else {
                LogError("[Controller] Failed to notify inject process - retrying once");
                // Retry once on failure
                if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                    LogInfo("[Controller] Recording started (retry)");
                } else {
                    LogError("[Controller] Recording start failed");
                    g_Recording = false;
                    PublishRecordingStartIntent(RecordingStartIntent::Idle, "inject start command failure");
                }
            }
        } else {
            LogError("[Controller] Inject process is not connected, cannot start recording");
            g_Recording = false;
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "inject unavailable");
        }
    } else {
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "record stop hotkey");
        WithInjectSharedMem([&](SharedMemoryLayout* shm) {
            shm->runtimeState.notificationType.store(
                static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing), std::memory_order_release);
            shm->runtimeState.notificationExpiry.store(GetTickCount64() + 60000ULL, std::memory_order_release);
        });
        if (g_PseudoOverlay) {
            g_PseudoOverlay->ShowRecordingFinalizingNotification();
        }
        LogInfo("[Controller] Stopping recording...");

        const bool stopAccepted = RequestRecordingStopAndReleaseMedia("record hotkey", 5000);
        if (stopAccepted) {
            LogInfo("[Controller] Recording stop accepted; media finalization continues asynchronously");
        } else {
            LogWarn("[Controller] Recording stop state cleared, but media finalization acceptance is unknown");
        }
    }

    if (g_Tray)
        g_Tray->SetRecordingState(g_Recording);
}

// Audio-only recording toggle (no video capture/encoding)
void ToggleAudioOnlyRecording() {
    g_Recording = !g_Recording;

    if (g_Recording) {
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
            g_Recording = false;
            if (g_Tray)
                g_Tray->SetRecordingState(false);
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "audio-only media readiness failure");
            return;
        }

        if (!audioOnlySet) {
            LogWarn("[Controller] No inject shared memory found for audio-only flag - media will use separate IPC");
            PublishRecordingStartIntent(RecordingStartIntent::AudioOnly, "audio-only shared-state retry");
        }

        bool startCommandDelivered = false;

        // Notify inject process - it sets cmdStartRecording in shared memory.
        // The audio-only flag and pending intent were already published above.
        if (g_InjectClient && g_InjectClient->IsConnected()) {
            ProcessResponse resp;
            if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr, &resp, 5000)) {
                startCommandDelivered = true;
                LogInfo("[Controller] Audio-only recording request delivered through inject");
            } else {
                LogWarn("[Controller] Failed to notify inject for audio-only recording; trying media directly");
            }
        }

        // Also notify media directly. This is authoritative when inject is unavailable
        // and harmless when the shared-memory request won the race first.
        if (g_MediaClient && g_MediaClient->IsConnected()) {
            ProcessResponse resp = ProcessResponse::Error;
            if (g_MediaClient->SendCommand(ProcessCommand::StartRecording, "audio_only", &resp, 5000) &&
                resp != ProcessResponse::Error) {
                startCommandDelivered = true;
                LogInfo("[Controller] Audio-only recording request accepted by media");
            }
        }
        if (!startCommandDelivered) {
            LogError("[Controller] Audio-only recording start failed");
            g_Recording = false;
            PublishRecordingStartIntent(RecordingStartIntent::Idle, "audio-only start command failure");
        }
    } else {
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "audio-only stop hotkey");
        WithInjectSharedMem([&](SharedMemoryLayout* shm) {
            shm->runtimeState.notificationType.store(
                static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing), std::memory_order_release);
            shm->runtimeState.notificationExpiry.store(GetTickCount64() + 60000ULL, std::memory_order_release);
        });
        if (g_PseudoOverlay) {
            g_PseudoOverlay->ShowRecordingFinalizingNotification();
        }
        LogInfo("[Controller] Stopping audio-only recording...");

        const bool stopAccepted = RequestRecordingStopAndReleaseMedia("audio-only hotkey", 5000);
        if (stopAccepted) {
            LogInfo("[Controller] Audio-only recording stop accepted; media finalization continues asynchronously");
        } else {
            LogWarn("[Controller] Audio-only stop state cleared, but media finalization acceptance is unknown");
        }
    }

    if (g_Tray)
        g_Tray->SetRecordingState(g_Recording);
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

    if (g_hLimiterProcess) {
        handles[handleCount] = g_hLimiterProcess;
        handleNames[handleCount++] = "Limiter";
    }
    if (g_hMediaProcess) {
        handles[handleCount] = g_hMediaProcess;
        handleNames[handleCount++] = "Media";
    }
    if (g_hInjectProcess) {
        handles[handleCount] = g_hInjectProcess;
        handleNames[handleCount++] = "Inject";
    }
    if (g_hLoggerProcess) {
        handles[handleCount] = g_hLoggerProcess;
        handleNames[handleCount++] = "Logger";
    }
    if (g_hSensorProcess) {
        handles[handleCount] = g_hSensorProcess;
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
            if (g_hLimiterProcess)
                TerminateProcess(g_hLimiterProcess, 1);
            if (g_hMediaProcess)
                TerminateProcess(g_hMediaProcess, 1);
            if (g_hInjectProcess)
                TerminateProcess(g_hInjectProcess, 1);
            if (g_hLoggerProcess)
                TerminateProcess(g_hLoggerProcess, 1);
            if (g_hSensorProcess)
                TerminateProcess(g_hSensorProcess, 1);
        } else {
            LogInfo("[Controller] All child processes exited cleanly");
        }
    }

    // Cleanup handles
    if (g_hLimiterProcess)
        CloseHandle(g_hLimiterProcess);
    if (g_hMediaProcess)
        CloseHandle(g_hMediaProcess);
    if (g_hInjectProcess)
        CloseHandle(g_hInjectProcess);
    if (g_hLoggerProcess)
        CloseHandle(g_hLoggerProcess);
    if (g_hSensorProcess)
        CloseHandle(g_hSensorProcess);

    g_hLimiterProcess = NULL;
    g_hMediaProcess = NULL;
    g_hInjectProcess = NULL;
    g_hLoggerProcess = NULL;
    g_hSensorProcess = NULL;
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

    RecordingStartIntent recordingStartIntent = g_RecordingStartIntent.load(std::memory_order_acquire);
    if (g_Recording && recordingStartIntent != RecordingStartIntent::Idle) {
        WithInjectSharedMem([&](SharedMemoryLayout* sharedMemory) {
            if (sharedMemory->runtimeState.isRecording.load(std::memory_order_acquire)) {
                recordingStartIntent = RecordingStartIntent::Idle;
                g_RecordingStartIntent.store(RecordingStartIntent::Idle, std::memory_order_release);
            }
        });
    }

    const bool recordingStartPending = g_Recording && recordingStartIntent != RecordingStartIntent::Idle;
    const bool mediaUnavailable = recordingStartPending &&
                                  (!g_hMediaProcess || !IsProcessRunning(g_hMediaProcess) || !g_MediaClient ||
                                   !g_MediaClient->IsConnected());
    const bool injectUnavailable = recordingStartPending && recordingStartIntent == RecordingStartIntent::Video &&
                                   (!g_hInjectProcess || !IsProcessRunning(g_hInjectProcess) || !g_InjectClient ||
                                    !g_InjectClient->IsConnected());
    const bool limiterUnavailable =
        recordingStartPending && recordingStartIntent == RecordingStartIntent::Video &&
        g_Config.fpsLimiter.captureSyncEnabled &&
        (!g_hLimiterProcess || !IsProcessRunning(g_hLimiterProcess) || !g_LimiterClient ||
         !g_LimiterClient->IsConnected());
    if (mediaUnavailable || injectUnavailable || limiterUnavailable) {
        const char* failedChild = mediaUnavailable ? "media" : (injectUnavailable ? "inject" : "limiter");
        LogError("[Controller] Required %s process/channel exited before recording became live; cancelling start intent",
                 failedChild);
        RequestRecordingStopAndReleaseMedia("required child exited before recording live", 1000);
        g_Recording = false;
        PublishRecordingStartIntent(RecordingStartIntent::Idle, "required child exited before recording live");
        if (g_Tray) {
            g_Tray->SetRecordingState(false);
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
    recoverProcess(ProcessMode::Inject, g_hInjectProcess, g_InjectClient.get(), "inject", true, injectRecoveryFailure);
    recoverProcess(ProcessMode::Media, g_hMediaProcess, g_MediaClient.get(), "media", g_hMediaProcess != nullptr,
                   mediaRecoveryFailure);
    recoverProcess(ProcessMode::Limiter, g_hLimiterProcess, g_LimiterClient.get(), "limiter",
                   g_hLimiterProcess != nullptr, limiterRecoveryFailure);
}

bool CompleteControllerStartup() {
    if (g_ControllerStartupTiming.complete) {
        return true;
    }

    LogInfo("[Controller] Completing deferred startup...");
    LogInfo("[Controller] Spawning child processes...");

    const int64_t injectSpawnStartUs = Log_GetQpcUs();
    g_hInjectProcess = SpawnChildProcess(ProcessMode::Inject, g_ConfigPath.c_str(), g_InjectClient.get());
    const int64_t injectSpawnUs = Log_GetQpcUs() - injectSpawnStartUs;
    if (!g_hInjectProcess) {
        LogError("[Controller] Failed to spawn inject process");
        return false;
    }

    int64_t mediaSpawnUs = 0;
    if (ShouldStartMediaProcessAtStartup()) {
        PrepareRecordingDiagnosticIdentity();
        const int64_t mediaSpawnStartUs = Log_GetQpcUs();
        g_hMediaProcess = SpawnChildProcess(ProcessMode::Media, g_ConfigPath.c_str(), g_MediaClient.get());
        mediaSpawnUs = Log_GetQpcUs() - mediaSpawnStartUs;
        if (!g_hMediaProcess) {
            LogError("[Controller] Failed to spawn media process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring media process startup until recording begins");
    }

    int64_t limiterSpawnUs = 0;
    if (ShouldStartLimiterProcessAtStartup(g_Config)) {
        const int64_t limiterSpawnStartUs = Log_GetQpcUs();
        g_hLimiterProcess = SpawnChildProcess(ProcessMode::Limiter, g_ConfigPath.c_str(), g_LimiterClient.get());
        limiterSpawnUs = Log_GetQpcUs() - limiterSpawnStartUs;
        if (!g_hLimiterProcess) {
            LogError("[Controller] Failed to spawn limiter process");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring limiter process startup until a limiter is enabled");
    }

    const int64_t auxSpawnStartUs = Log_GetQpcUs();
    if (ShouldStartLoggerProcess(g_Config)) {
        g_hLoggerProcess = SpawnChildProcess(ProcessMode::Logger, g_ConfigPath.c_str());
        if (!g_hLoggerProcess) {
            LogError("[Controller] Failed to spawn logger process");
        }
    }
    if (ShouldStartSensorProcess(g_Config)) {
        g_hSensorProcess = SpawnChildProcess(ProcessMode::Sensors, g_ConfigPath.c_str());
        if (!g_hSensorProcess) {
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

    if (!g_DeferredLaunchPath.empty()) {
        LogInfo("[Controller] Launching deferred game: %s", g_DeferredLaunchPath.c_str());
        LaunchGameSuspended(g_DeferredLaunchPath);
    }

    LogInfo("[Controller] Registering hotkeys...");
    const int64_t hotkeyStartUs = Log_GetQpcUs();
