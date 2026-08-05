#include "media_main_internal.h"

bool StartRecording(const AppConfig& config) {
    if (media_main_g_Recording)
        return true;

    media_main_g_PrivacyFailClosedStopRequested.store(false, std::memory_order_release);
    ResetRecordingHealthPublication();
    LogInfo("[Media] Starting recording...");

    timeBeginPeriod(1);

    // A prior interrupted session must never leave hook-side video publication
    // armed. The selected live path below explicitly enables it only for inject.
    SetInjectVideoCaptureRequestedState(false, "recording start reset");

    if (media_main_g_AudioOnly) {
        LogInfo("[Media] Audio-only recording mode - skipping video capture");

        // Clear any stale shared memory state
        if (media_main_g_pSharedMem) {
            StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStartRecording, false);
            StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStopRecording, false);
            StoreRelease(media_main_g_pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::None));
            SetRecordingVisibleState(false);
        }

        if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
            LogError("[Media] Failed to start MediaEngine audio-only recording");
            SetRecordingVisibleState(false);
            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                         "MediaEngine audio-only initialization");
            timeEndPeriod(1);
            media_main_g_AudioOnly = false;
            return false;
        }

        media_main_g_Recording = true;
        SetRecordingVisibleState(true);
        SetCapturePipelinePhase(CapturePipelinePhase::kLive);

        LogInfo("[Media] Audio-only recording active");
        return true;
    }

    if (IsVideoCaptureDisabledMethod(config.captureMethod)) {
        LogError("[Media] Video recording is disabled by the active application profile");
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                     "active profile has video_capture=none");
        timeEndPeriod(1);
        return false;
    }

    bool useScreenGrab = IsPreferredScreenGrab();
    if (IsScreenGrabCaptureMethod(config.captureMethod)) {
        useScreenGrab = true;
    } else if (IsInjectCaptureMethod(config.captureMethod)) {
        useScreenGrab = false;
    }
    SetActiveScreenGrab(useScreenGrab);

    if (useScreenGrab && !media_main_g_WgcCap) {
        LogError("[Media] WGC capture requested but no WGC target is available");
        SetActiveScreenGrab(false);
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "WGC target unavailable");
        timeEndPeriod(1);
        return false;
    }

    if (!useScreenGrab && media_main_g_pSharedMem) {
        const uint32_t writeIndex = media_main_g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
        const uint32_t readIndex = media_main_g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
        if (!IsFrameRingWindowValid(writeIndex, readIndex)) {
            LogError(
                "[Media] Inject recording rejected corrupt shared frame ring "
                "(write=%u read=%u distance=%u version=%u ABI=0x%08X)",
                writeIndex, readIndex, static_cast<uint32_t>(writeIndex - readIndex),
                media_main_g_pSharedMem->GetVersion(), media_main_g_pSharedMem->abiSignature.load(std::memory_order_acquire));
            StoreRelease(media_main_g_pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::SharedMemoryProtocolIntegrity));
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            PublishRecordingStartFailure(RecordingFailureCode::SharedMemoryProtocolIntegrity,
                                         "corrupt shared frame ring");
            timeEndPeriod(1);
            return false;
        }
    }

    // Clear any stale shared memory commands/state from previous (possibly crashed)
    // recording sessions. If a previous media process crashed, cmdStopRecording
    // may still be true, causing the new recording to stop immediately.
    if (media_main_g_pSharedMem) {
        StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStartRecording, false);
        StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStopRecording, false);
        StoreRelease(media_main_g_pSharedMem->runtimeState.recordingFailureCode,
                     static_cast<uint32_t>(RecordingFailureCode::None));
        SetRecordingVisibleState(false);
    }

    // Reset inject session state so main loop re-initializes on new recording
    media_main_g_InjectSessionReset.store(true, std::memory_order_release);

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    ResetInjectFrameRingToLatest("recording start");

    media_main_g_FrameQueue.Clear();
    ResetLastQueuedFrameCache();
    media_main_g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
    media_main_g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    media_main_g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
    media_main_g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
    media_main_g_InjectDeferredFrames.store(0, std::memory_order_relaxed);
    media_main_g_ActivePathMismatchFramesDiscarded.store(0, std::memory_order_relaxed);

    if (media_main_g_pSharedMem) {
        ResetRuntimeDiagnostics(media_main_g_pSharedMem);
        media_main_g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    EnsureInjectCaptureEvents();
    if (media_main_g_InjectCaptureShutdownEvent) {
        ResetEvent(media_main_g_InjectCaptureShutdownEvent);
    }
    if (media_main_g_InjectFrameReadyEvent) {
        ResetEvent(media_main_g_InjectFrameReadyEvent);
    }

    SetInjectVideoCaptureRequestedState(!useScreenGrab,
                                        useScreenGrab ? "screen-grab recording path" : "inject recording path");
    SetCaptureRequestedState(true);

    if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
        LogError("[Media] Failed to start MediaEngine recording");
        SetInjectVideoCaptureRequestedState(false, "recording start failure");
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "MediaEngine initialization");
        timeEndPeriod(1);
        return false;
    }

    if (useScreenGrab) {
        // Last point before any frame of this recording can be captured. Screen-grab
        // capture records the composited desktop, so CE's own recording-start status has
        // to be off screen now: the encoder thread's warmup reservoir is captured from
        // here on and is handed to the live output intact.
        RequestStatusOverlayDarkForCapture("screen-grab capture start");
    }

    media_main_g_Recording = true;
    media_main_g_EncoderRunning = true;
    media_main_g_RecordingUsesVfr.store(config.video.useVFR, std::memory_order_release);
    media_main_g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
    media_main_g_CfrDrainStopQpc.store(0, std::memory_order_release);

    // Recording-lifetime config snapshot (see StartWgcRecordingCapture): the
    // main thread may reassign `config` mid-recording; encoder-thread settings
    // are fixed per session by design, so it reads an owned copy.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        media_main_g_EncoderThread = std::thread([configSnapshot]() { EncoderThreadFunc(*configSnapshot); });
    }

    if (useScreenGrab && media_main_g_WgcCap) {
        if (!StartWgcRecordingCapture(config)) {
            LogError("[Media] Failed to start WGC capture");
            media_main_g_EncoderRunning = false;
            JoinThreadWithTimeout(media_main_g_EncoderThread, 10000, "encoder");
            media_main_g_Recording = false;
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            MediaEngine_StopRecording(true);
            SetActiveScreenGrab(false);
            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "WGC capture initialization");
            timeEndPeriod(1);
            return false;
        }
        LogInfo("[Media] Active recording path: %s bounded pull-drain CFR (%d fps output)",
                media_main_g_WgcCap->IsUsingDesktopDuplication() ? "DXGI-duplication" : "WGC", config.video.fps);
    } else if (!useScreenGrab) {
        if (config.captureMethod == "auto" && media_main_g_WgcCap && media_main_g_AutoWgcFallbackArmed.load(std::memory_order_acquire)) {
            LogInfo("[Media] Active recording path: inject shared-memory capture (WGC auto-fallback armed)");
        } else {
            LogInfo("[Media] Active recording path: inject shared-memory capture");
        }
        ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        media_main_g_InjectCaptureShutdown = false;
        EnsureInjectCaptureEvents();
        // Recording-lifetime config snapshot (see StartWgcRecordingCapture).
        {
            auto configSnapshot = std::make_shared<const AppConfig>(config);
            media_main_g_InjectCaptureThread = std::thread([configSnapshot]() { InjectCaptureThreadFunc(*configSnapshot); });
        }
    }

    LogInfo("[Media] Recording warmup armed");
    return true;
}

void StopRecording() {
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
    }
    if (!media_main_g_Recording)
        return;

    LogInfo("[Media] Stopping recording...");

    SetInjectVideoCaptureRequestedState(false, "recording stop");

    // Audio-only: skip all video/capture cleanup
    if (media_main_g_AudioOnly) {
        media_main_g_Recording = false;
        SetRecordingVisibleState(false);
        SetCapturePipelinePhase(CapturePipelinePhase::kStopping);

        const bool outputSaved = MediaEngine_StopRecording(false);
        CompleteRecordingFinalization(false, outputSaved);

        if (media_main_g_pSharedMem) {
            ResetRuntimeDiagnostics(media_main_g_pSharedMem);
            media_main_g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
            media_main_g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
        }

        SetActiveScreenGrab(false);
        media_main_g_AudioOnly = false;
        timeEndPeriod(1);
        LogInfo("[Media] Audio-only recording stopped");
        return;
    }

    const bool wasActiveScreenGrab = IsActiveScreenGrab();
    const bool recordingUsesVfr = media_main_g_RecordingUsesVfr.load(std::memory_order_acquire);
    media_main_g_Recording = false;
    const CapturePipelinePhase stopTransition = BeginCapturePipelineStop();
    const bool cancelBeforeLive = stopTransition == CapturePipelinePhase::kCancelling;
    const bool drainOutstandingCfrTicks =
        !cancelBeforeLive &&
        ce::capture_policy::ShouldDrainOutstandingCfrTicksAtStop(wasActiveScreenGrab, recordingUsesVfr);
    LogInfo("[RecordingLifecycle] Stop accepted as %s (path=%s liveFrames=%u)",
            cancelBeforeLive ? "pre-live cancellation" : "live finalization",
            wasActiveScreenGrab ? "WGC" : "inject",
            media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_acquire) : 0u);
    int64_t drainStopQpc = 0;
    if (drainOutstandingCfrTicks) {
        LARGE_INTEGER stopQpc;
        QueryPerformanceCounter(&stopQpc);
        drainStopQpc = stopQpc.QuadPart;
    }

    SetCaptureRequestedState(false);
    SetRecordingVisibleState(false);
    media_main_g_CfrDrainStopQpc.store(drainStopQpc, std::memory_order_release);
    media_main_g_DrainOutstandingCfrTicks.store(drainOutstandingCfrTicks && drainStopQpc > 0, std::memory_order_release);
    if (wasActiveScreenGrab) {
        media_main_g_EncoderRunning = false;
    }
    if (drainOutstandingCfrTicks && drainStopQpc > 0) {
        LogInfo("[Media] CFR stop drain armed at QPC=%lld path=%s", drainStopQpc,
                IsActiveScreenGrab() ? "WGC" : "inject");
    } else if (wasActiveScreenGrab && recordingUsesVfr) {
        LogInfo("[Media] WGC VFR exact-stop: no CFR debt to drain");
    }

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    if (!wasActiveScreenGrab) {
        media_main_g_EncoderRunning = false;
    }

    media_main_g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
    media_main_g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

    const bool encoderJoined = JoinThreadWithTimeout(media_main_g_EncoderThread, 60000, "encoder");

    if (wasActiveScreenGrab && !encoderJoined) {
        LogWarn("[Media] WGC encoder join timed out after exact-stop shutdown");
    }

    media_main_g_FrameQueue.Clear();
    ResetLastQueuedFrameCache();
    ResetInjectFrameRingToLatest("recording stop");

    if (media_main_g_pSharedMem) {
        // Recording has stopped, so zero-copy encoder textures must not stay
        // live. Clear the handshake before stopping the encoder so the DLL
        // tears down all preserved D3D11/KMT resources immediately.
        media_main_g_pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        media_main_g_pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        media_main_g_pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);

    }

    const bool outputSaved = MediaEngine_StopRecording(cancelBeforeLive);
    CompleteRecordingFinalization(cancelBeforeLive, outputSaved);
    if (MediaEngine_ReleaseEncoderTextures) {
        MediaEngine_ReleaseEncoderTextures();
    }
    // Reset WGC-specific 10-bit hint so inject-mode recordings don't inherit it.
    if (MediaEngine_SetSourcePrefers10Bit) {
        MediaEngine_SetSourcePrefers10Bit(false);
    }
    media_main_g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);

    if (media_main_g_pSharedMem) {
        ResetRuntimeDiagnostics(media_main_g_pSharedMem);
        media_main_g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    media_main_g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
    media_main_g_CfrDrainStopQpc.store(0, std::memory_order_release);
    media_main_g_RecordingUsesVfr.store(false, std::memory_order_release);
    SetActiveScreenGrab(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LogInfo("[Media] Recording stopped");
    timeEndPeriod(1);
}

int MediaProcessMain(const AppConfig& initialConfig) {
    AppConfig config = initialConfig;
    media_main_g_RecordingManifestLogPath =
        IsAnyLoggingEnabled(initialConfig.logLevel) ? initialConfig.logFilePath : std::string{};
    Log_SetLevel(config.logLevel);
    SetConsoleCtrlHandler(MediaConsoleHandler, TRUE);

    // Get exe directory for DLL loading
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    const std::string configPath = GetLocalConfigPath();
    // Directory for the per-device render-endpoint latency cache (next to the config).
    std::string mediaCacheDir = configPath;
    {
        const size_t slash = mediaCacheDir.find_last_of("\\/");
        mediaCacheDir = (slash != std::string::npos) ? mediaCacheDir.substr(0, slash) : std::string();
    }
    bool mediaEngineReady = false;

    auto unloadMediaEngineIdle = [&]() {
        if (mediaEngineReady && MediaEngine_Shutdown) {
            MediaEngine_Shutdown();
            mediaEngineReady = false;
        }
        MediaEngine_Unload();
        LogInfo("[Media] MediaEngine unloaded for idle state");
    };

    auto ensureMediaEngineReady = [&]() -> bool {
        // When switching to audio-only mode with an already-initialized engine,
        // force reinit so Init() skips the VideoEncoder and creates the audio-only muxer.
        if (mediaEngineReady && media_main_g_AudioOnly) {
            LogInfo("[Media] Re-initializing MediaEngine for audio-only mode");
            if (MediaEngine_Shutdown) {
                MediaEngine_Shutdown();
            }
            MediaEngine_Unload();
            mediaEngineReady = false;
            // Fall through to full init below
        }

        if (mediaEngineReady) {
            return true;
        }

        if (!MediaEngine_Load(exeDir.c_str())) {
            LogError("[Media] Failed to load mediaengine.dll");
            return false;
        }

        MediaEngine_SetLogCallback(IsDebugLoggingEnabled(config.logLevel) ? MediaLogCallback : nullptr);
        // Propagate audio-only flag to MediaEngine before Init
        if (media_main_g_AudioOnly && MediaEngine_SetAudioOnly) {
            MediaEngine_SetAudioOnly(true);
        }
        // Auto-detect the render-domain delay before Init. The product-safe path is audio-only:
        // no calibration window, no WGC/DX stimulus, and no config.ini delay writeback. Apply the
        // result (or explicit low-confidence reason) before the media engine snapshots config.
        if (!media_main_g_Recording.load(std::memory_order_acquire)) {
            MeasureRenderLatencyOnce(config, mediaCacheDir);
        }
        ApplyAutoDetectedRenderLatencyToConfig(config);

        if (!MediaEngine_Init(&config)) {
            LogError("[Media] Failed to initialize MediaEngine");
            if (MediaEngine_Shutdown) {
                MediaEngine_Shutdown();
            }
            MediaEngine_Unload();
            return false;
        }

        if (media_main_g_pSharedMem || media_main_g_pShmem) {
            MediaEngine_SetSharedMem(media_main_g_pSharedMem, media_main_g_pShmem);
        }

        mediaEngineReady = true;
        LogInfo("[Media] MediaEngine initialized");
        return true;
    };

    ApplyMediaPrioritySettings(config);

    ProcessIPCServer ipc(ProcessMode::Media);
    if (!ipc.Init()) {
        LogError("[Media] Failed to initialize IPC");
        return 1;
    }
    // Authenticated owner of the controller-side status overlay; keys the status-sync and
    // capture-dark events so they can never bind to a foreign process.
    ce::status_overlay::SetControllerPid(ipc.ControllerPid());

    if (!ensureMediaEngineReady()) {
        return 1;
    }

    LogInfo("[Media] SharedMemory Layout Check:");
    LogInfo("[Media] sizeof(FrameSlot) = %zu", sizeof(FrameSlot));
    LogInfo("[Media] sizeof(CaptureState) = %zu", sizeof(CaptureState));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    LogInfo("[Media] offsetof(frameRing) = %zu", offsetof(SharedMemoryLayout, frameRing));
    LogInfo("[Media] offsetof(runtimeState) = %zu", offsetof(SharedMemoryLayout, runtimeState));
#pragma GCC diagnostic pop

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;

    auto isExplicitInjectConfig = [&]() -> bool { return IsInjectCaptureMethod(config.captureMethod); };
    auto isExplicitWgcConfig = [&]() -> bool { return IsWgcCaptureMethod(config.captureMethod); };
    auto isExplicitDxgiDupConfig = [&]() -> bool { return IsDxgiDupCaptureMethod(config.captureMethod); };
    // Any explicit non-inject screen-grab family method (wgc or dxgi_dup).
    auto isExplicitScreenGrabConfig = [&]() -> bool { return IsScreenGrabCaptureMethod(config.captureMethod); };
    auto isAutoCaptureConfig = [&]() -> bool { return IsAutoCaptureMethod(config.captureMethod); };
    auto setWgcPreferenceAfterFailure = [&]() {
        SetPreferredScreenGrab(isExplicitScreenGrabConfig() || isAutoCaptureConfig());
    };
    auto isInjectCaptureTarget = [&](const std::string& processName) -> bool {
        if (const ApplicationProfile* profile = FindApplicationProfileForProcess(config, processName)) {
            return profile->resolvedVideoCapture == ApplicationVideoCapture::kInject;
        }
        const bool gameWhitelistMatched =
            !processName.empty() && MatchesProcessEntries(config.gameWhitelist, processName);
        return ce::capture_policy::ShouldUseInjectCaptureForAutoTarget(isExplicitInjectConfig(), isAutoCaptureConfig(),
                                                                       gameWhitelistMatched);
    };
    auto resolveSourceProcessName = [&](uint32_t sourcePid, const std::string& knownName = std::string{}) {
        if (!knownName.empty() && knownName != "unknown") {
            return knownName;
        }
        if (sourcePid == 0) {
            return std::string{};
        }
        std::string resolvedName = GetProcessNameFromPID(sourcePid);
        return resolvedName == "unknown" ? std::string{} : resolvedName;
    };
    auto isInjectCaptureTargetForSource = [&](uint32_t sourcePid, const std::string& knownName = std::string{}) {
        return isInjectCaptureTarget(resolveSourceProcessName(sourcePid, knownName));
    };

    if (isExplicitScreenGrabConfig()) {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Using %s mode (explicit)", isExplicitDxgiDupConfig() ? "DXGI duplication" : "WGC");
    }

    LogInfo("[Media] Attempting to connect to shared memory...");

    for (int retry = 0; retry < 10 && !media_main_g_pSharedMem; retry++) {
        HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDiscovery) {
            DiscoveryInfo* pDiscovery =
                (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (ValidateDiscoveryInfo(pDiscovery) && pDiscovery->GetInjectPid() != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDiscovery->GetInjectPid());

                media_main_g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (media_main_g_hMapFile) {
                    media_main_g_pSharedMem = (SharedMemoryLayout*)MapViewOfFile(media_main_g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                      sizeof(SharedMemoryLayout));

                    if (media_main_g_pSharedMem && ValidateSharedMemory(media_main_g_pSharedMem) && media_main_g_pSharedMem->GetHostPID() != 0) {
                        LogInfo("[Media] Connected via discovery (inject PID: %u, ABI: 0x%08X)",
                                pDiscovery->injectPid.load(), SHARED_MEMORY_ABI_SIGNATURE);
                        UnmapViewOfFile(pDiscovery);
                        CloseHandle(hDiscovery);
                        break;
                    }

                    if (media_main_g_pSharedMem) {
                        LogError(
                            "[Media] Rejected shared memory header: magic=0x%08X version=%u size=%u abi=0x%08X "
                            "expected=(0x%08X,%u,%zu,0x%08X)",
                            media_main_g_pSharedMem->GetMagic(), media_main_g_pSharedMem->GetVersion(),
                            media_main_g_pSharedMem->structSize.load(std::memory_order_acquire),
                            media_main_g_pSharedMem->abiSignature.load(std::memory_order_acquire), SHARED_MEMORY_MAGIC,
                            SHARED_MEMORY_VERSION, sizeof(SharedMemoryLayout), SHARED_MEMORY_ABI_SIGNATURE);
                    }

                    if (media_main_g_pSharedMem) {
                        UnmapViewOfFile(media_main_g_pSharedMem);
                        media_main_g_pSharedMem = nullptr;
                    }
                    CloseHandle(media_main_g_hMapFile);
                    media_main_g_hMapFile = NULL;
                }
                UnmapViewOfFile(pDiscovery);
            }
            CloseHandle(hDiscovery);
        }

        if (!media_main_g_pSharedMem) {
            Sleep(50);
        }
    }

    if (media_main_g_pSharedMem) {
        if (media_main_g_pSharedMem->GetShmemMappingCreated()) {
            wchar_t shmemName[64];
            GenerateShmemName(shmemName, 64, media_main_g_pSharedMem->GetHostPID());
            media_main_g_hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
            if (media_main_g_hMapShmem) {
                size_t mapSize = media_main_g_pSharedMem->GetShmemMappingSize();
                media_main_g_pShmem = (ShmemBuffer*)MapViewOfFile(media_main_g_hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
                if (media_main_g_pShmem) {
                    LogInfo("[Media] Connected to separate Shmem mapping '%ls' (mapped %zu bytes)", shmemName, mapSize);
                }
            }
        }

        if (mediaEngineReady) {
            MediaEngine_SetSharedMem(media_main_g_pSharedMem, media_main_g_pShmem);
        }

        if (isExplicitScreenGrabConfig()) {
            SetPreferredScreenGrab(true);
            LogInfo("[Media] Connected to shared memory - using %s for capture",
                    isExplicitDxgiDupConfig() ? "DXGI duplication" : "WGC");
        } else {
            SetPreferredScreenGrab(false);
            LogInfo("[Media] Connected to shared memory - using inject mode");
        }
    } else if (isExplicitInjectConfig() && config.profileWgcTargets.empty() &&
               config.profileDxgiDupTargets.empty()) {
        LogError("[Media] Failed to connect to shared memory in inject mode!");
        unloadMediaEngineIdle();
        return 1;
    } else {
        SetPreferredScreenGrab(true);
        LogInfo("[Media] Shared memory not available - using a configured screen-grab route");
    }

    auto applyWgcOptions = [&](WGCCapture* capture) {
        if (!capture) {
            return;
        }
        capture->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
        capture->SetSameDeviceCapture(config.wgcSameDeviceCapture);
        capture->SetAllowLossyBgra8Pool(config.wgcAllowLossyBgra8Pool);
        capture->SetVideoMemoryReservationMode(config.wgcVideoMemoryReservation);
    };

    if (IsPreferredScreenGrab() || isAutoCaptureConfig()) {
        if (!ensureMediaEngineReady()) {
            return 1;
        }
        d3dDevice = MediaEngine_GetD3D11Device();
        if (!d3dDevice) {
            if (IsPreferredScreenGrab()) {
                LogError("[Media] Failed to get D3D11 device");
                unloadMediaEngineIdle();
                return 1;
            }
        } else {
            ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
            d3dDevice->GetImmediateContext(&d3dContext);

            // Do not create a default-primary WGC item here. Target resolution
            // belongs to the recording route below so stale/eager setup can
            // never become an unintended cross-monitor source.
            LogInfo("[Media] Screen-grab device ready; capture target initialization deferred until routing");
        }
    }

    LogInfo("[Media] Process started (PID: %lu) Mode: %s", GetCurrentProcessId(),
            IsPreferredScreenGrab() ? "WGC" : "inject");
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    int64_t recordingStartTime = 0;
    auto ensureWgcDevice = [&]() -> bool {
        if (!ensureMediaEngineReady()) {
            return false;
        }
        if (d3dDevice) {
            return true;
        }
        d3dDevice = MediaEngine_GetD3D11Device();
        if (!d3dDevice) {
            return false;
        }
        ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
        if (!d3dContext) {
            d3dDevice->GetImmediateContext(&d3dContext);
        }
        return true;
    };
    auto releaseIdleWgcResources = [&]() {
        StopWgcCapturePipeline();
        PublishWgcCapture(nullptr, "idle WGC resource release");
        if (d3dContext) {
            d3dContext->ClearState();
            d3dContext->Flush();
            d3dContext->Release();
            d3dContext = nullptr;
        }
        d3dDevice = nullptr;
        if (MediaEngine_ReleaseSharedD3D11Device) {
            MediaEngine_ReleaseSharedD3D11Device();
        }
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        LogInfo("[Media] Released idle WGC D3D11 resources");
    };
    DWORD lastEarlyWgcScan = 0;
    DWORD lastWindowScanTime = 0;
    HWND currentCapturedWindow = NULL;
    std::string currentCapturedMonitorStableId;
    bool currentTargetPrefersInject = false;
    WgcRetargetRequest pendingWgcRetarget;
    uint32_t lastSourcePid = 0;
    uint32_t activeConfigSourcePid = 0;
    std::string activeConfigProcessName;
    ce::capture_handoff::InjectToWgcHandoff autoWgcHandoff;
    uint32_t autoWgcHandoffBaselineFrames = 0;
    uint64_t autoWgcHandoffDeadlineTick = 0;
    constexpr uint64_t kAutoWgcHandoffReadyTimeoutMs = 2000;

    auto clearCurrentWgcTarget = [&]() {
        currentCapturedWindow = NULL;
        currentCapturedMonitorStableId.clear();
        currentTargetPrefersInject = false;
        pendingWgcRetarget = {};
    };

    auto discardCurrentWgcTarget = [&](const char* reason) {
        PublishWgcCapture(nullptr, reason);
        clearCurrentWgcTarget();
    };

    auto queueWgcRetarget = [&](HWND targetWindow, HMONITOR targetMonitor, bool preferMonitor, const char* reason) {
        pendingWgcRetarget.window = targetWindow;
        pendingWgcRetarget.monitor = targetMonitor;
        pendingWgcRetarget.preferMonitor = preferMonitor || targetWindow == NULL;
        pendingWgcRetarget.active = true;
        LogWarn("[Media] Queued WGC retarget: %s (window=0x%p monitor=0x%p monitorOnly=%d)", reason, targetWindow,
                targetMonitor, pendingWgcRetarget.preferMonitor ? 1 : 0);
    };

    auto refreshActiveConfig = [&](bool forceReload, HWND targetWindow = NULL, uint32_t confirmedPid = 0,
                                   const std::string& confirmedProcessName = std::string{}) -> std::string {
        uint32_t sourcePid = 0;
        std::string processName;
        if (media_main_g_pSharedMem) {
            sourcePid = media_main_g_pSharedMem->GetSourcePid();
            if (sourcePid != 0) {
                processName = GetProcessNameFromPID(sourcePid);
                if (processName.empty() && sourcePid == activeConfigSourcePid)
                    processName = activeConfigProcessName;
            }
        }
        if (sourcePid == 0) {
            HWND resolvedWindow = targetWindow;
            if (!resolvedWindow && currentCapturedWindow && IsWindow(currentCapturedWindow))
                resolvedWindow = currentCapturedWindow;
            if (resolvedWindow) {
                DWORD resolvedPid = 0;
                GetWindowThreadProcessId(resolvedWindow, &resolvedPid);
                sourcePid = resolvedPid;
            }
            if (sourcePid != 0) {
                if (sourcePid == confirmedPid && !confirmedProcessName.empty())
                    processName = confirmedProcessName;
                else
                    processName = GetProcessNameFromPID(sourcePid);
                if (processName.empty() && sourcePid == activeConfigSourcePid)
                    processName = activeConfigProcessName;
            }
        }
        if (sourcePid == 0 && media_main_g_Recording && activeConfigSourcePid != 0) {
            sourcePid = activeConfigSourcePid;
            processName = activeConfigProcessName;
        }

        if (!forceReload && sourcePid == activeConfigSourcePid && processName == activeConfigProcessName) {
            return processName;
        }

        AppConfig resolvedConfig;
        LoadConfig(configPath, resolvedConfig);
        if (!processName.empty()) {
            LoadConfig(configPath, resolvedConfig, processName);
        }
        // Normalize runtime-only sync state before comparing with the active config. Otherwise a
        // config.ini reload with no real media changes would compare file defaults (usually 0 ms)
        // against the active auto-detected render latency and reload unnecessarily.
        ApplyAutoDetectedRenderLatencyToConfig(resolvedConfig);

        const bool mediaConfigChanged = !MediaEngineConfigEquals(config, resolvedConfig);

        config = std::move(resolvedConfig);
        Log_SetLevel(config.logLevel);
        activeConfigSourcePid = sourcePid;
        activeConfigProcessName = processName;

        if (media_main_g_Recording && IsActiveScreenGrab())
            PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice, true, "active profile refresh");

        ApplyMediaPrioritySettings(config);
        if (d3dDevice) {
            ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
        } else {
            ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        }
        if (auto capture = media_main_g_WgcCap.Read()) {
            applyWgcOptions(capture.get());
            capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
        }
        if (mediaEngineReady) {
            MediaEngine_SetLogCallback(IsDebugLoggingEnabled(config.logLevel) ? MediaLogCallback : nullptr);
            if (forceReload || mediaConfigChanged) {
                MediaEngine_ReloadConfig(&config);
            }
            if (media_main_g_pSharedMem || media_main_g_pShmem) {
                MediaEngine_SetSharedMem(media_main_g_pSharedMem, media_main_g_pShmem);
            }
        }
        return processName;
    };

    auto markInjectPreferredTarget = [&](HWND targetWindow, uint32_t sourcePid, const char* reason) -> bool {
        if (isExplicitScreenGrabConfig()) {
            return false;
        }

        if (!isInjectCaptureTargetForSource(sourcePid)) {
            return false;
        }

        if (!ShouldPreferInjectCaptureForFullscreenWindow(targetWindow, sourcePid)) {
            return false;
        }

        if (currentTargetPrefersInject && currentCapturedWindow == targetWindow) {
            SetPreferredScreenGrab(false);
            return true;
        }

        currentCapturedWindow = targetWindow;
        currentCapturedMonitorStableId.clear();
        currentTargetPrefersInject = true;
        SetPreferredScreenGrab(false);
        LogInfo("[Media] %s: hooked fullscreen-like window 0x%p will use inject capture instead of WGC", reason,
                targetWindow);
        return true;
    };

    auto primeWgcMonitorTarget = [&](HMONITOR targetMonitor) -> bool {
        if (isExplicitInjectConfig() || !targetMonitor) {
            return false;
        }

        // Monitor-scope backend priority: DXGI Desktop Duplication is the
        // preferred pure desktop/monitor capture path (explicit dxgi_dup, or
        // auto mode where no inject/window target exists). Explicit wgc keeps
        // the WGC monitor item; duplication failures always fall back to WGC.
        const bool preferDuplication = ce::capture_policy::ShouldPreferDxgiDuplicationForMonitorCapture(
            isExplicitDxgiDupConfig(), isExplicitWgcConfig(), isAutoCaptureConfig());

        {
            const auto existingCapture = media_main_g_WgcCap.Read();
            HWND existingWindow = NULL;
            HMONITOR existingMonitor = NULL;
            if (existingCapture)
                existingCapture->GetTargetIdentity(&existingWindow, &existingMonitor);
            if (targetMonitor == existingMonitor && existingWindow == NULL && currentCapturedWindow == NULL &&
                !currentTargetPrefersInject && existingCapture &&
                existingCapture->IsUsingDesktopDuplication() == preferDuplication) {
                applyWgcOptions(existingCapture.get());
                existingCapture->SetCaptureCursor(
                    ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
                existingCapture->SetThrottleFlag(nullptr);
                SetPreferredScreenGrab(true);
                return true;
            }
        }

        if (!ensureWgcDevice()) {
            return false;
        }

        auto capture = std::make_shared<WGCCapture>();
        applyWgcOptions(capture.get());
        bool initOk = false;
        if (preferDuplication) {
            initOk = capture->InitForMonitorDuplication(d3dDevice, targetMonitor);
            if (initOk) {
                LogInfo("[Media] Monitor capture backend selected: DXGI duplication (%s)",
                        isExplicitDxgiDupConfig() ? "explicit capture_method=dxgi_dup" : "auto desktop fallback");
            } else {
                LogWarn(
                    "[Media] DXGI duplication monitor target unavailable (monitor=0x%p); "
                    "falling back to WGC monitor capture",
                    targetMonitor);
            }
        }
        if (!initOk && targetMonitor) {
            initOk = capture->InitForMonitor(d3dDevice, targetMonitor);
            if (!initOk) {
                LogWarn("[Media] Failed to init WGC for selected monitor 0x%p; refusing cross-monitor fallback",
                        targetMonitor);
            }
        }
        if (!initOk) {
            return false;
        }

        capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
        capture->SetThrottleFlag(nullptr);
        PublishWgcCapture(std::move(capture), "monitor retarget");
        SetPreferredScreenGrab(true);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        return true;
    };

    auto primeConfiguredMonitorTarget = [&](HWND targetWindow, HMONITOR targetHint, const std::string& selectorText,
                                            const char* context) -> bool {
        ce::monitor_selection::Selector selector;
        if (!ce::monitor_selection::TryParseSelector(selectorText, selector)) {
            LogError("[CaptureTarget] invalid normalized monitor selector '%s' (%s)", selectorText.c_str(), context);
            return false;
        }

        HWND foregroundWindow = NULL;
        if (selector.kind == ce::monitor_selection::SelectorKind::kAuto) {
            const ForegroundWgcWindowCandidate foreground = GetForegroundWgcWindowCandidate();
            if (foreground.usable)
                foregroundWindow = foreground.hwnd;
        }

        ce::monitor_selection::ResolveRequest request;
        request.selector = selector;
        request.targetWindow = targetWindow;
        request.hint = targetHint;
        request.foregroundWindow = foregroundWindow;
        const ce::monitor_selection::ResolveResult resolved = ce::monitor_selection::Resolve(request);
        if (!resolved) {
            LogError(
                "[CaptureTarget] monitor resolution failed: selector=%s context=%s error=%s; refusing fallback to "
                "another display",
                selector.canonical.c_str(), context, resolved.error.c_str());
            return false;
        }

        const ce::monitor_selection::MonitorDescriptor& monitor = resolved.descriptor;
        LogInfo(
            "[CaptureTarget] resolved selector=%s reason=%s context=%s monitor=0x%p id=%s device=%s name=%s "
            "bounds=(%ld,%ld)-(%ld,%ld) primary=%d adapter=%08X:%08X source=%u target=%u",
            selector.canonical.c_str(), resolved.reason.c_str(), context, resolved.monitor, monitor.stableId.c_str(),
            monitor.deviceName.c_str(), monitor.friendlyName.c_str(), monitor.desktopRect.left, monitor.desktopRect.top,
            monitor.desktopRect.right, monitor.desktopRect.bottom, monitor.primary ? 1 : 0,
            static_cast<uint32_t>(monitor.adapterLuid.HighPart), monitor.adapterLuid.LowPart, monitor.sourceId,
            monitor.targetId);
        if (!primeWgcMonitorTarget(resolved.monitor))
            return false;
        currentCapturedMonitorStableId = monitor.stableId;
        return true;
    };

    auto primePinnedMonitorTarget = [&](HMONITOR previousMonitor, const char* context) -> bool {
        if (!currentCapturedMonitorStableId.empty()) {
            return primeConfiguredMonitorTarget(NULL, NULL, "id:" + currentCapturedMonitorStableId, context);
        }
        return primeConfiguredMonitorTarget(NULL, previousMonitor, "auto", context);
    };

    auto monitorSelectorIsExplicit = [](const std::string& selectorText) {
        ce::monitor_selection::Selector selector;
        return ce::monitor_selection::TryParseSelector(selectorText, selector) &&
               ce::monitor_selection::IsExplicitSelector(selector);
    };

    // Fullscreen-game duplication priming: capture the MONITOR that hosts the
    // game window through monitor-scope capture so the live hardware cursor plane
    // is preserved (WGC sessions demote the cursor to DWM-composed rendering;
    // see wgc-capture.md). A duplication failure may use WGC for the same
    // resolved monitor, but never a different display.
    auto primeDxgiDupForWindowMonitor = [&](HWND targetWindow, const std::string& selectorText,
                                            const char* reason) -> bool {
        if (isExplicitInjectConfig() || !targetWindow) {
            return false;
        }
        if (!primeConfiguredMonitorTarget(targetWindow, NULL, selectorText, reason))
            return false;

        const auto capture = media_main_g_WgcCap.Read();
        const bool usingDuplication = capture && capture->IsUsingDesktopDuplication();
        LogInfo(
            "[Media] Fullscreen game target captured from its selected monitor via %s (%s hwnd=0x%p)",
            usingDuplication ? "DXGI duplication" : "same-monitor WGC fallback", reason, targetWindow);
        return true;
    };

    auto primeWgcWindowTarget = [&](HWND targetWindow, bool logPrimed, bool allowMonitorFallback = true) -> bool {
        if (isExplicitInjectConfig()) {
            return false;
        }

        if (!targetWindow) {
            return false;
        }

        {
            const auto existingCapture = media_main_g_WgcCap.Read();
            if (currentCapturedWindow == targetWindow && existingCapture && !currentTargetPrefersInject) {
                applyWgcOptions(existingCapture.get());
                existingCapture->SetCaptureCursor(
                    ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
                existingCapture->SetThrottleFlag(nullptr);
                SetPreferredScreenGrab(true);
                return true;
            }
        }

        if (!ensureWgcDevice()) {
            setWgcPreferenceAfterFailure();
            return false;
        }

        auto capture = std::make_shared<WGCCapture>();
        applyWgcOptions(capture.get());
        if (capture->InitForWindow(d3dDevice, targetWindow)) {
            capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));

            capture->SetThrottleFlag(nullptr);
            PublishWgcCapture(std::move(capture), "window retarget");
            SetPreferredScreenGrab(true);
            currentCapturedWindow = targetWindow;
            currentCapturedMonitorStableId.clear();
            currentTargetPrefersInject = false;
            if (logPrimed) {
                LogInfo("[Media] WGC target primed for window 0x%p", targetWindow);
            }
            return true;
        }

        LogError("[Media] Failed to init WGC for found window 0x%p.", targetWindow);
        currentCapturedWindow = NULL;
        currentTargetPrefersInject = false;
        if (!allowMonitorFallback) {
            return false;
        }

        LogWarn("[Media] Falling back to WGC monitor capture after window init failure");
        if (!primeConfiguredMonitorTarget(targetWindow, NULL, config.captureMonitor, "WGC window fallback")) {
            setWgcPreferenceAfterFailure();
            discardCurrentWgcTarget("WGC window monitor fallback unavailable");
            return false;
        }
        return true;
    };

    auto applyPendingWgcRetarget = [&]() -> bool {
        if (!pendingWgcRetarget.active) {
            return false;
        }

        WgcRetargetRequest request = pendingWgcRetarget;
        pendingWgcRetarget = {};

        const bool restartActiveCapture = media_main_g_Recording && IsActiveScreenGrab();
        const auto previousCapture = media_main_g_WgcCap.Load();
        const HWND previousCapturedWindow = currentCapturedWindow;
        const std::string previousCapturedMonitorStableId = currentCapturedMonitorStableId;
        const bool previousTargetPrefersInject = currentTargetPrefersInject;
        const bool previousPreferredScreenGrab = IsPreferredScreenGrab();
        if (restartActiveCapture) {
            StopWgcCapturePipeline();
        }

        auto restorePreviousCapture = [&](const char* failureReason) -> bool {
            if (!previousCapture) {
                LogError("[Media] WGC retarget rollback unavailable (%s): no previous capture", failureReason);
                return false;
            }
            const auto publishedCapture = media_main_g_WgcCap.Load();
            if (publishedCapture.get() != previousCapture.get()) {
                PublishWgcCapture(previousCapture, "retarget rollback");
            }
            currentCapturedWindow = previousCapturedWindow;
            currentCapturedMonitorStableId = previousCapturedMonitorStableId;
            currentTargetPrefersInject = previousTargetPrefersInject;
            SetPreferredScreenGrab(previousPreferredScreenGrab);
            if (restartActiveCapture && !StartWgcRecordingCapture(config)) {
                LogError("[Media] WGC retarget rollback failed to restart previous source (%s)", failureReason);
                return false;
            }
            LogWarn("[Media] WGC retarget rolled back to previous source (%s)", failureReason);
            return true;
        };

        if (!request.preferMonitor && request.window && !IsWindow(request.window)) {
            request.window = NULL;
            request.preferMonitor = true;
        }

        bool primed = false;
        if (!request.preferMonitor && request.window) {
            primed = primeWgcWindowTarget(request.window, true);
        }
        if (!primed) {
            primed = primePinnedMonitorTarget(request.monitor, "runtime monitor retarget");
        }
        if (!primed) {
            LogWarn("[Media] Failed to initialize queued WGC retarget; restoring previous source");
            restorePreviousCapture("replacement initialization failed");
            return false;
        }

        if (restartActiveCapture) {
            if (!StartWgcRecordingCapture(config)) {
                LogError("[Media] Failed to start replacement WGC capture; restoring previous source");
                restorePreviousCapture("replacement start failed");
                return false;
            }
            LogInfo("[Media] WGC capture restarted after retarget");
        }
        return true;
    };

    auto prepareCaptureForRecordingStart = [&]() {
        std::string processName = refreshActiveConfig(false);
        uint32_t sourcePid = media_main_g_pSharedMem ? media_main_g_pSharedMem->GetSourcePid() : 0;
        bool injectWhitelisted = isInjectCaptureTarget(processName);

        autoWgcHandoff.Reset();
        autoWgcHandoffBaselineFrames = 0;
        autoWgcHandoffDeadlineTick = 0;
        media_main_g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

        int profileWgcScore = std::numeric_limits<int>::min();
        int profileDxgiScore = std::numeric_limits<int>::min();
        uint32_t profileWgcPid = 0;
        uint32_t profileDxgiPid = 0;
        std::string profileWgcProcessName;
        std::string profileDxgiProcessName;
        WhitelistEntry profileWgcTarget;
        WhitelistEntry profileDxgiTarget;
        HWND profileWgcWindow = FindMatchingWgcWindow(config.profileWgcTargets, &profileWgcScore, true,
                                                      &profileWgcPid, &profileWgcProcessName, &profileWgcTarget);
        HWND profileDxgiWindow = FindMatchingWgcWindow(config.profileDxgiDupTargets, &profileDxgiScore, true,
                                                       &profileDxgiPid, &profileDxgiProcessName, &profileDxgiTarget);
        const bool useProfileDxgi = profileDxgiWindow && (!profileWgcWindow || profileDxgiScore > profileWgcScore);
        HWND profileWindow = useProfileDxgi ? profileDxgiWindow : profileWgcWindow;
        if (profileWindow) {
            const uint32_t profilePid = useProfileDxgi ? profileDxgiPid : profileWgcPid;
            const std::string& profileProcessName =
                useProfileDxgi ? profileDxgiProcessName : profileWgcProcessName;
            if (sourcePid == 0) {
                sourcePid = profilePid;
            }
            processName = refreshActiveConfig(false, profileWindow, profilePid, profileProcessName);
            injectWhitelisted = isInjectCaptureTarget(processName);
            if (useProfileDxgi) {
                std::string profileMonitorSelector = config.captureMonitor;
                if (const ApplicationProfile* profile = FindApplicationProfileForTarget(config, profileDxgiTarget)) {
                    profileMonitorSelector = profile->captureMonitor;
                }
                ce::monitor_selection::Selector parsedProfileSelector;
                const bool explicitProfileMonitor =
                    ce::monitor_selection::TryParseSelector(profileMonitorSelector, parsedProfileSelector) &&
                    ce::monitor_selection::IsExplicitSelector(parsedProfileSelector);
                if (primeDxgiDupForWindowMonitor(profileWindow, profileMonitorSelector, "application profile")) {
                    const auto profileCapture = media_main_g_WgcCap.Read();
                    LogInfo("[Media] Application profile selected monitor capture via %s (process=%s hwnd=0x%p)",
                            profileCapture && profileCapture->IsUsingDesktopDuplication() ? "DXGI duplication"
                                                                                          : "same-monitor WGC",
                            processName.empty() ? "unknown" : processName.c_str(), profileWindow);
                    return;
                }
                if (explicitProfileMonitor) {
                    LogError(
                        "[Media] Application profile's explicit monitor target is unavailable; refusing window or "
                        "primary fallback");
                    SetPreferredScreenGrab(true);
                    discardCurrentWgcTarget("explicit profile monitor unavailable");
                    return;
                }
                if (primeWgcWindowTarget(profileWindow, false, false)) {
                    LogWarn("[Media] Application profile DXGI target unavailable; using WGC window capture");
                    return;
                }
                LogWarn("[Media] Application profile DXGI and WGC targets failed; continuing fallback selection");
            } else {
                if (primeWgcWindowTarget(profileWindow, false, false)) {
                    LogInfo("[Media] Application profile selected WGC window capture (process=%s hwnd=0x%p)",
                            processName.empty() ? "unknown" : processName.c_str(), profileWindow);
                    return;
                }
                LogWarn("[Media] Application profile WGC target failed to initialize; continuing fallback selection");
            }
        }

        if (isExplicitInjectConfig()) {
            SetPreferredScreenGrab(false);
            clearCurrentWgcTarget();
            return;
        }

        if (isAutoCaptureConfig() && injectWhitelisted) {
            // Auto mode prefers inject for known-compatible games, but a hook
            // connection is not proof that frames will arrive. Keep a fully
            // initialized screen-grab target for the SAME game/window/monitor
            // as a startup fallback. The generic primary-monitor standby is not
            // sufficient on multi-monitor systems.
            bool fallbackReady = false;
            HWND fallbackWindow = sourcePid != 0 ? GetMainWindowForProcess(sourcePid) : NULL;
            if (!fallbackWindow) {
                const ForegroundWgcWindowCandidate candidate = GetForegroundWgcWindowCandidate();
                // A foreground window is a valid fallback only when it belongs
                // to the requested source process (or no source PID is known).
                // Otherwise a transiently missing game window could silently
                // redirect an auto recording to an unrelated foreground app.
                if (candidate.usable && (sourcePid == 0 || candidate.pid == sourcePid)) {
                    fallbackWindow = candidate.hwnd;
                }
            }
            HMONITOR fallbackMonitor =
                fallbackWindow ? MonitorFromWindow(fallbackWindow, MONITOR_DEFAULTTONEAREST) : NULL;
            if (fallbackWindow && IsWindowFullscreenLike(fallbackWindow) && config.autoFullscreenPrefersDxgiDup) {
                fallbackReady =
                    primeDxgiDupForWindowMonitor(fallbackWindow, config.captureMonitor, "inject startup fallback");
            }
            const bool explicitMonitorFallbackFailed =
                !fallbackReady && fallbackWindow && IsWindowFullscreenLike(fallbackWindow) &&
                config.autoFullscreenPrefersDxgiDup && monitorSelectorIsExplicit(config.captureMonitor);
            if (explicitMonitorFallbackFailed) {
                LogError(
                    "[CaptureTarget] explicit monitor startup fallback is unavailable; leaving inject fallback "
                    "unarmed instead of selecting another display or window");
            }
            if (!fallbackReady && !explicitMonitorFallbackFailed && fallbackWindow) {
                fallbackReady = primeWgcWindowTarget(fallbackWindow, false, false);
            }
            if (!fallbackReady && !explicitMonitorFallbackFailed && fallbackMonitor && WGCCapture::IsSupported()) {
                fallbackReady = primeConfiguredMonitorTarget(fallbackWindow, fallbackMonitor, config.captureMonitor,
                                                               "inject startup monitor fallback");
            }
            if (!fallbackReady && !fallbackWindow) {
                LogWarn(
                    "[Media] Auto inject fallback could not resolve a window/monitor for source PID %lu; "
                    "leaving fallback unarmed instead of capturing an unrelated primary monitor",
                    static_cast<unsigned long>(sourcePid));
            }
            media_main_g_AutoWgcFallbackArmed.store(fallbackReady, std::memory_order_release);
            SetPreferredScreenGrab(false);
            LogInfo(
                "[Media] Injection whitelist matched %s; auto mode will use inject capture "
                "(WGC fallback=%s hwnd=0x%p hmon=0x%p)",
                processName.c_str(), fallbackReady ? "armed" : "unavailable", fallbackWindow, fallbackMonitor);
            return;
        }

        if (!config.wgcWindowTitles.empty()) {
            if (isExplicitDxgiDupConfig()) {
                LogInfo("[Media] wgc_window_detection ignored: capture_method=dxgi_dup is monitor-scope only");
            } else {
                HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
                if (matchedWindow) {
                    if (markInjectPreferredTarget(matchedWindow, sourcePid, "WGC title match")) {
                        return;
                    }
                    if (primeWgcWindowTarget(matchedWindow, false, false)) {
                        LogInfo(
                            "[Media] WGC window detection matched configured target; WGC window capture selected "
                            "(hwnd=0x%p)",
                            matchedWindow);
                        return;
                    }
                    LogWarn(
                        "[Media] WGC configured window target 0x%p failed to initialize; continuing fallback selection",
                        matchedWindow);
                }
            }
        }

        if (sourcePid != 0 && MatchesProcessEntries(config.overlayWhitelist, processName)) {
            if (!ensureWgcDevice()) {
                LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                setWgcPreferenceAfterFailure();
                discardCurrentWgcTarget("overlay capture device unavailable");
                return;
            }

            SetPreferredScreenGrab(true);
            HWND hGameWindow = GetMainWindowForProcess(sourcePid);
            if (isExplicitDxgiDupConfig()) {
                if (!primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                  "overlay target DXGI monitor capture")) {
                    setWgcPreferenceAfterFailure();
                    discardCurrentWgcTarget("overlay DXGI monitor unavailable");
                }
                return;
            }
            if (hGameWindow) {
                LogInfo("[Media] Overlay-only hook target %s; WGC capture selected", processName.c_str());
                if (!primeWgcWindowTarget(hGameWindow, false, false)) {
                    LogWarn("[Media] Overlay-only WGC window target 0x%p failed to initialize; falling back to monitor",
                            hGameWindow);
                    if (!primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                      "overlay target monitor fallback")) {
                        setWgcPreferenceAfterFailure();
                        discardCurrentWgcTarget("overlay target monitor fallback unavailable");
                    }
                }
            } else if (!primeConfiguredMonitorTarget(NULL, NULL, config.captureMonitor,
                                                     "overlay target without a window")) {
                setWgcPreferenceAfterFailure();
                discardCurrentWgcTarget("overlay monitor unavailable");
            }
            return;
        }

        if (isExplicitScreenGrabConfig()) {
            HWND sourceWindow = sourcePid != 0 ? GetMainWindowForProcess(sourcePid) : NULL;
            if (!primeConfiguredMonitorTarget(sourceWindow, NULL, config.captureMonitor,
                                              "explicit monitor-scope capture")) {
                setWgcPreferenceAfterFailure();
                discardCurrentWgcTarget("explicit monitor unavailable");
            }
            return;
        }

        if (currentCapturedWindow != NULL && !IsWindow(currentCapturedWindow)) {
            clearCurrentWgcTarget();
        }

        if (isAutoCaptureConfig()) {
            if (sourcePid != 0) {
                HWND hGameWindow = GetMainWindowForProcess(sourcePid);
                const bool monitorRouteRequested =
                    hGameWindow && ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                                       isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                                       IsWindowFullscreenLike(hGameWindow), config.autoFullscreenPrefersDxgiDup);
                if (monitorRouteRequested) {
                    if (primeDxgiDupForWindowMonitor(hGameWindow, config.captureMonitor, "unhooked source window"))
                        return;
                    if (monitorSelectorIsExplicit(config.captureMonitor)) {
                        LogError("[CaptureTarget] explicit auto-mode monitor target failed; refusing window fallback");
                        SetPreferredScreenGrab(true);
                        discardCurrentWgcTarget("explicit auto monitor unavailable");
                        return;
                    }
                }
                if (hGameWindow && primeWgcWindowTarget(hGameWindow, false, false)) {
                    LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                            processName.empty() ? "target" : processName.c_str());
                    return;
                }
            }

            ForegroundWgcWindowCandidate foregroundCandidate = GetForegroundWgcWindowCandidate();
            const bool matchedConfiguredWgcWindow = false;
            if (ce::capture_policy::ShouldPreferForegroundFullscreenWindowForAutoWgc(
                    isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted, sourcePid != 0,
                    matchedConfiguredWgcWindow, foregroundCandidate.usable, foregroundCandidate.fullscreenLike)) {
                const bool monitorRouteRequested =
                    ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                        isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                        foregroundCandidate.fullscreenLike, config.autoFullscreenPrefersDxgiDup);
                if (monitorRouteRequested) {
                    if (primeDxgiDupForWindowMonitor(foregroundCandidate.hwnd, config.captureMonitor,
                                                     "foreground fullscreen window"))
                        return;
                    if (monitorSelectorIsExplicit(config.captureMonitor)) {
                        LogError(
                            "[CaptureTarget] explicit foreground monitor target failed; refusing window fallback");
                        SetPreferredScreenGrab(true);
                        discardCurrentWgcTarget("explicit foreground monitor unavailable");
                        return;
                    }
                }
                if (primeWgcWindowTarget(foregroundCandidate.hwnd, false, false)) {
                    LogInfo(
                        "[Media] Auto mode: no source PID; foreground fullscreen WGC window capture selected "
                        "(pid=%lu process=%s hwnd=0x%p)",
                        static_cast<unsigned long>(foregroundCandidate.pid), foregroundCandidate.processName.c_str(),
                        foregroundCandidate.hwnd);
                    return;
                }
                LogWarn(
                    "[Media] Auto mode: foreground fullscreen WGC window init failed "
                    "(pid=%lu process=%s hwnd=0x%p); falling back to monitor capture",
                    static_cast<unsigned long>(foregroundCandidate.pid), foregroundCandidate.processName.c_str(),
                    foregroundCandidate.hwnd);
            } else if (sourcePid == 0) {
                LogInfo(
                    "[Media] Auto mode: foreground WGC window candidate not used "
                    "(usable=%d fullscreenLike=%d matchedConfiguredWindow=%d configuredEntries=%zu)",
                    foregroundCandidate.usable ? 1 : 0, foregroundCandidate.fullscreenLike ? 1 : 0,
                    matchedConfiguredWgcWindow ? 1 : 0, config.wgcWindowTitles.size());
            }

            if (primeConfiguredMonitorTarget(foregroundCandidate.usable ? foregroundCandidate.hwnd : NULL, NULL,
                                             config.captureMonitor, "auto monitor fallback")) {
                LogInfo("[Media] Auto mode: no inject whitelist match; WGC monitor capture selected");
            } else {
                setWgcPreferenceAfterFailure();
                discardCurrentWgcTarget("auto monitor target unavailable");
                LogWarn("[Media] Auto mode: WGC target unavailable and inject capture is not allowed for this target");
            }
            return;
        }

        SetPreferredScreenGrab(false);
    };

    while (media_main_g_Running) {
        // WGC window detection must run BEFORE resolution polling/texture creation.
        // CreateSharedCaptureTextures sets the encoder's LUID device, which conflicts
        // with WGC's shared device. By scanning first, the preferred capture mode is set correctly
        // and we skip the LUID-based texture creation for WGC games.
        if (mediaEngineReady && media_main_g_pSharedMem && isAutoCaptureConfig() && !config.wgcWindowTitles.empty() &&
            !IsPreferredScreenGrab() && !media_main_g_Recording) {
            DWORD now = GetTickCount();
            if (now - lastEarlyWgcScan > 500) {
                lastEarlyWgcScan = now;
                HWND matchedWindow = FindMatchingWgcWindow(config.wgcWindowTitles);
                if (matchedWindow) {
                    uint32_t sourcePid = media_main_g_pSharedMem->GetSourcePid();
                    if (!markInjectPreferredTarget(matchedWindow, sourcePid, "Early WGC scan")) {
                        if (!primeWgcWindowTarget(matchedWindow, false, false)) {
                            LogWarn("[Media] Early WGC window scan matched 0x%p but window init failed", matchedWindow);
                        }
                    }
                }
            }
        }

        // Poll for resolution availability and create encoder textures early.
        // The Vulkan layer sets resolution when it creates the swapchain, then waits
        // for encoder KMT textures. We must create them ASAP to avoid timeout.
        // Skip when using screengrab (WGC) - the encoder should use the shared device.
        if (media_main_g_Recording && media_main_g_pSharedMem && !IsPreferredScreenGrab() &&
            !media_main_g_pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
            uint32_t w = media_main_g_pSharedMem->GetWidth();
            uint32_t h = media_main_g_pSharedMem->GetHeight();
            uint32_t f = media_main_g_pSharedMem->GetFormat();
            if (w > 0 && h > 0) {
                LogInfo("[Media] Resolution available (%dx%d fmt=%d), creating encoder textures", w, h, f);
                if (MediaEngine_CreateSharedCaptureTextures(w, h, f, media_main_g_pSharedMem)) {
                    LogInfo("[Media] Encoder KMT textures created (main loop)");
                }
            }
        }

        ProcessCommand cmd;
        char cmdPayload[256] = {};
        if (ipc.PollCommand(cmd, cmdPayload, sizeof(cmdPayload))) {
            switch (cmd) {
                case ProcessCommand::Shutdown:
                    LogInfo("[Media] Shutdown command received");
                    media_main_g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                case ProcessCommand::StartRecording: {
                    media_main_g_AudioOnly = (strcmp(cmdPayload, "audio_only") == 0);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for recording start");
                        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                     "MediaEngine reinitialization");
                        media_main_g_AudioOnly = false;
                        ipc.SendResponse(ProcessResponse::Error, "recording_start_failed");
                        break;
                    }
                    prepareCaptureForRecordingStart();
                    PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice,
                                                 !media_main_g_AudioOnly && IsPreferredScreenGrab(), "recording start");
                    const bool started = StartRecording(config);
                    if (!started)
                        PublishMediaScreenGrabTarget(0, nullptr, false, "recording start failure");
                    media_main_g_AudioOnly = false;  // Reset after StartRecording consumed it
                    ipc.SendResponse(started ? ProcessResponse::RecordingStarted : ProcessResponse::Error,
                                     started ? nullptr : "recording_start_failed");
                    break;
                }
                case ProcessCommand::StopRecording:
                    // A direct stop owns all hook-facing state. Do this even if
                    // startup had not yet reached g_Recording, and consume any
                    // older shared-memory command before this process exits.
                    if (media_main_g_pSharedMem) {
                        StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStartRecording, false);
                        StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStopRecording, false);
                    }
                    SetInjectVideoCaptureRequestedState(false, "authenticated stop request");
                    SetCaptureRequestedState(false);
                    SetRecordingVisibleState(false);
                    PublishMediaScreenGrabTarget(0, nullptr, false, "authenticated stop request");
                    // Accept the authenticated request before potentially lengthy
                    // encoder/mux finalization. The controller may then release its
                    // endpoint while this disposable media process finishes and exits.
                    if (!ipc.SendResponse(ProcessResponse::Ack))
                        LogWarn("[Media] Failed to acknowledge graceful recording stop; finalizing anyway");
                    StopRecording();
                    releaseIdleWgcResources();
                    // Exit after recording stops to free GPU VRAM.
                    // Controller respawns on next recording via EnsureMediaProcessReady.
                    LogInfo("[Media] Recording finished, exiting to release GPU resources");
                    media_main_g_Running = false;
                    break;
                case ProcessCommand::Ping:
                    ipc.SendResponse(ProcessResponse::Pong);
                    break;
                case ProcessCommand::ReloadConfig: {
                    refreshActiveConfig(true);
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                }
                default:
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
            }
        }
        if (ipc.HasFatalDisconnect()) {
            LogWarn("[Media] Controller IPC disconnected; stopping for a clean respawn");
            if (media_main_g_Recording)
                StopRecording();
            media_main_g_Running = false;
            break;
        }

        if (media_main_g_PrivacyFailClosedStopRequested.exchange(false, std::memory_order_acq_rel) && media_main_g_Recording) {
            LogError("[PrivacyBlackout] Owner thread is stopping the recording after a fail-closed video error");
            StopRecording();
        }

        if (media_main_g_pSharedMem) {
            if (media_main_g_WgcCap && media_main_g_Recording && IsActiveScreenGrab() && media_main_g_WgcCap->NeedsReset()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                media_main_g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                const std::string resetReason = media_main_g_WgcCap->ConsumeResetReason();
                queueWgcRetarget(currentWindow, currentMonitor, currentWindow == NULL,
                                 resetReason.empty() ? "runtime reset requested" : resetReason.c_str());
            }

            if (media_main_g_WgcCap && media_main_g_Recording && IsActiveScreenGrab() && media_main_g_WgcCap->IsWindowTarget() &&
                !media_main_g_WgcCap->IsTargetWindowValid()) {
                HWND currentWindow = NULL;
                HMONITOR currentMonitor = NULL;
                media_main_g_WgcCap->GetTargetIdentity(&currentWindow, &currentMonitor);
                queueWgcRetarget(NULL, currentMonitor, true, "target window invalid during recording");
            }

            if (pendingWgcRetarget.active && media_main_g_Recording && IsActiveScreenGrab()) {
                applyPendingWgcRetarget();
            }

            if (LoadAcquire(media_main_g_pSharedMem->runtimeState.cmdStartRecording)) {
                StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStartRecording, false);
                if (!media_main_g_Recording) {
                    // Check audioOnly flag from shared memory (set by controller for audio-only via inject)
                    media_main_g_AudioOnly = LoadAcquire(media_main_g_pSharedMem->runtimeState.audioOnly);
                    // Always clear after consuming to prevent stale carry-over
                    StoreRelease(media_main_g_pSharedMem->runtimeState.audioOnly, false);
                    if (!ensureMediaEngineReady()) {
                        LogError("[Media] Failed to reinitialize MediaEngine for shared-memory recording start");
                        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                     "shared-memory MediaEngine reinitialization");
                    } else {
                        prepareCaptureForRecordingStart();
                        PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice,
                                                     !media_main_g_AudioOnly && IsPreferredScreenGrab(),
                                                     "shared-memory recording start");
                        const bool started = StartRecording(config);
                        if (!started)
                            PublishMediaScreenGrabTarget(0, nullptr, false, "shared-memory start failure");
                        media_main_g_AudioOnly = false;
                        if (started) {
                            media_main_g_pSharedMem->runtimeState.ackRecordingStarted.store(true, std::memory_order_release);
                        }
                    }
                }
            }
            if (LoadAcquire(media_main_g_pSharedMem->runtimeState.cmdStopRecording)) {
                StoreRelease(media_main_g_pSharedMem->runtimeState.cmdStopRecording, false);
                if (media_main_g_Recording) {
                    PublishMediaScreenGrabTarget(0, nullptr, false, "shared-memory stop request");
                    StopRecording();
                    releaseIdleWgcResources();
                    media_main_g_pSharedMem->runtimeState.ackRecordingStopped.store(true, std::memory_order_release);
                }
                // Exit after recording stops to free GPU VRAM.
                LogInfo("[Media] Recording finished (shmem), exiting to release GPU resources");
                media_main_g_Running = false;
            }

            DWORD now = GetTickCount();
            if (!media_main_g_Recording && !isExplicitInjectConfig() && !isExplicitDxgiDupConfig() &&
                !config.wgcWindowTitles.empty() && (now - lastWindowScanTime > 1000)) {
                lastWindowScanTime = now;
                HWND foundWindow = FindMatchingWgcWindow(config.wgcWindowTitles);

                if (foundWindow) {
                    if (foundWindow != currentCapturedWindow) {
                        LogInfo(
                            "[Media] WGC Trigger: Found window (0x%p) matching config. "
                            "Switching capture...",
                            foundWindow);
                    }
                    if (markInjectPreferredTarget(foundWindow, media_main_g_pSharedMem->GetSourcePid(), "WGC trigger")) {
                        continue;
                    }
                    if (!primeWgcWindowTarget(foundWindow, foundWindow != currentCapturedWindow, false)) {
                        LogWarn("[Media] WGC trigger window 0x%p failed to initialize; falling back to monitor target",
                                foundWindow);
                        if (!primeConfiguredMonitorTarget(foundWindow, NULL, config.captureMonitor,
                                                          "configured WGC trigger fallback")) {
                            LogWarn("[Media] WGC trigger ignored: D3D11 device unavailable");
                        }
                    }
                } else if (!foundWindow && currentCapturedWindow != NULL) {
                    if (!IsWindow(currentCapturedWindow)) {
                        LogInfo(
                            "[Media] Captured window 0x%p no longer valid. Reverting "
                            "to monitor/inject.",
                            currentCapturedWindow);
                        clearCurrentWgcTarget();
                        if (!primeConfiguredMonitorTarget(NULL, NULL, config.captureMonitor,
                                                          "invalid WGC window fallback")) {
                            setWgcPreferenceAfterFailure();
                        }
                    }
                }
            }

            uint32_t currentSourcePid = media_main_g_pSharedMem->GetSourcePid();

            if (currentSourcePid != 0 && currentSourcePid != lastSourcePid) {
                lastSourcePid = currentSourcePid;
                std::string procName = refreshActiveConfig(false);
                if (procName.empty()) {
                    procName = GetProcessNameFromPID(currentSourcePid);
                }
                LogInfo("[Media] Hook connected: %s (PID: %u)", procName.c_str(), currentSourcePid);

                const bool injectWhitelisted = isInjectCaptureTarget(procName);
                const bool forceWGC = !isExplicitInjectConfig() && !injectWhitelisted &&
                                      MatchesProcessEntries(config.overlayWhitelist, procName);
                if (forceWGC) {
                    LogInfo("[Media] Overlay-only hook target %s connected; WGC capture remains selected",
                            procName.c_str());
                    if (media_main_g_Recording && !IsActiveScreenGrab()) {
                        LogInfo(
                            "[Media] Current recording stays on inject; WGC remains armed "
                            "only as a startup fallback");
                    }
                }

                if (media_main_g_Recording && !IsActiveScreenGrab() && injectWhitelisted &&
                    media_main_g_InjectDeliveredFirstFrame.load(std::memory_order_acquire) &&
                    media_main_g_AutoWgcFallbackArmed.exchange(false, std::memory_order_acq_rel)) {
                    LogInfo("[Media] Inject delivery confirmed for %s; disarming WGC startup fallback",
                            procName.c_str());
                }

                if (!media_main_g_Recording && forceWGC) {
                    HWND matchedWindow = (config.wgcWindowTitles.empty() || isExplicitDxgiDupConfig())
                                             ? NULL
                                             : FindMatchingWgcWindow(config.wgcWindowTitles);
                    if (matchedWindow) {
                        if (primeWgcWindowTarget(matchedWindow, false, false)) {
                            continue;
                        }
                        LogWarn(
                            "[Media] WGC configured window target 0x%p failed during pre-record scan; "
                            "continuing fallback selection",
                            matchedWindow);
                    }

                    if (!ensureWgcDevice()) {
                        LogWarn("[Media] Overlay whitelist requested WGC but D3D11 device unavailable");
                        setWgcPreferenceAfterFailure();
                        discardCurrentWgcTarget("connected overlay capture device unavailable");

                    } else {
                        SetPreferredScreenGrab(true);

                        HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                        if (isExplicitDxgiDupConfig()) {
                            if (!primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                              "connected overlay DXGI target")) {
                                LogError("[Media] Failed to initialize selected DXGI monitor target");
                                setWgcPreferenceAfterFailure();
                                discardCurrentWgcTarget("connected overlay DXGI monitor unavailable");
                            }
                            continue;
                        }
                        if (hGameWindow) {
                            LogInfo(
                                "[Media] Overlay-only target: found main window 0x%p. "
                                "Switching WGC to window mode.",
                                hGameWindow);

                            if (primeWgcWindowTarget(hGameWindow, false, false)) {
                                LogInfo("[Media] WGC window target primed for PID %u", currentSourcePid);
                            } else if (primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                                    "connected overlay monitor fallback")) {
                                LogWarn("[Media] Failed to init WGC for window - falling back to monitor capture");
                            } else {
                                LogError("[Media] Failed to init WGC for window or monitor");
                                setWgcPreferenceAfterFailure();
                            }
                        } else {
                            LogInfo(
                                "[Media] Whitelist: No main window found for PID %u. Using "
                                "Monitor Capture.",
                                currentSourcePid);
                            if (!primeConfiguredMonitorTarget(NULL, NULL, config.captureMonitor,
                                                              "connected overlay target without window")) {
                                setWgcPreferenceAfterFailure();
                            }
                        }
                    }

                } else if (!media_main_g_Recording && injectWhitelisted) {
                    SetPreferredScreenGrab(false);
                    clearCurrentWgcTarget();
                    LogInfo("[Media] Injection whitelist matched %s; using inject capture", procName.c_str());
                } else if (!media_main_g_Recording && isAutoCaptureConfig()) {
                    HWND hGameWindow = GetMainWindowForProcess(currentSourcePid);
                    const bool monitorRouteRequested =
                        hGameWindow && ce::capture_policy::ShouldPreferDxgiDuplicationForFullscreenAutoTarget(
                                           isAutoCaptureConfig(), isExplicitInjectConfig(), injectWhitelisted,
                                           IsWindowFullscreenLike(hGameWindow), config.autoFullscreenPrefersDxgiDup);
                    bool monitorRouteSelected = false;
                    if (monitorRouteRequested) {
                        monitorRouteSelected = primeDxgiDupForWindowMonitor(
                            hGameWindow, config.captureMonitor, "unhooked connected source window");
                        if (!monitorRouteSelected && monitorSelectorIsExplicit(config.captureMonitor)) {
                            LogError(
                                "[CaptureTarget] explicit connected-source monitor target failed; refusing window "
                                "fallback");
                            SetPreferredScreenGrab(true);
                            discardCurrentWgcTarget("explicit connected monitor unavailable");
                            continue;
                        }
                    }
                    if (monitorRouteSelected) {
                        // Selected monitor-scope capture for the game's display.
                    } else if (hGameWindow && primeWgcWindowTarget(hGameWindow, false, false)) {
                        LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC window capture selected",
                                procName.c_str());
                    } else if (primeConfiguredMonitorTarget(hGameWindow, NULL, config.captureMonitor,
                                                            "connected auto monitor fallback")) {
                        LogInfo("[Media] Auto mode: %s is not on the inject whitelist; WGC monitor capture selected",
                                procName.c_str());
                    } else {
                        setWgcPreferenceAfterFailure();
                        discardCurrentWgcTarget("connected auto monitor unavailable");
                        LogWarn("[Media] Auto mode: WGC target unavailable for %s; no inject capture fallback",
                                procName.c_str());
                    }
                } else if (!media_main_g_Recording && !isExplicitScreenGrabConfig()) {
                    SetPreferredScreenGrab(false);
                    LogInfo("[Media] Using Inject Mode (explicit/default)");
                }
            }
        }

        // Release preserved encoder textures once the hook signals it no longer uses them.
        // The Vulkan layer clears useEncoderTextures in CleanupCapture (vkDestroyDevice).
        if (!media_main_g_Recording && media_main_g_pSharedMem && MediaEngine_ReleaseEncoderTextures) {
            static bool lastUseEncoderTextures = false;
            bool curUseEncoderTextures = media_main_g_pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
            if (lastUseEncoderTextures && !curUseEncoderTextures) {
                LogInfo("[Media] Game released encoder textures - freeing preserved D3D11/VRAM resources");
                MediaEngine_ReleaseEncoderTextures();
            }
            lastUseEncoderTextures = curUseEncoderTextures;
        }

        bool hasPendingInputs = false;

        if (IsActiveScreenGrab() && media_main_g_Recording) {
            if (recordingStartTime == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                recordingStartTime = now.QuadPart;
            }

            if (media_main_g_pSharedMem) {
                media_main_g_pSharedMem->runtimeState.hostDroppedFrames.store(
                    static_cast<uint32_t>(media_main_g_FrameQueue.GetDroppedCount()), std::memory_order_relaxed);
            }
        } else if (!IsActiveScreenGrab() && media_main_g_Recording && media_main_g_pSharedMem) {
            FrameRingBuffer& ring = media_main_g_pSharedMem->frameRing;
            uint32_t wIdx = ring.writeIndex.load(std::memory_order_acquire);
            static uint32_t localReadIdx = 0;
            static bool receivedFirstFrame = false;
            static DWORD injectModeStartTime = 0;
            static bool sessionInitialized = false;

            static bool sharedTexturesCreated = false;

            // Reset session state when a new recording starts
            if (media_main_g_InjectSessionReset.exchange(false, std::memory_order_acq_rel)) {
                sessionInitialized = false;
                localReadIdx = 0;
                receivedFirstFrame = false;
                injectModeStartTime = 0;
                sharedTexturesCreated = false;
            }

            if (!sessionInitialized) {
                localReadIdx = wIdx;
                receivedFirstFrame = false;
                injectModeStartTime = 0;
                sessionInitialized = true;
                LogInfo("[Media] Inject mode session initialized, localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
            }

            static DWORD lastPollLog = 0;
            if (GetTickCount() - lastPollLog > 1000) {
                LogInfo("[Media] Polling: localReadIdx=%u, wIdx=%u", localReadIdx, wIdx);
                lastPollLog = GetTickCount();
            }

            if (injectModeStartTime == 0) {
                injectModeStartTime = GetTickCount();
                receivedFirstFrame = false;
            }

            if (!receivedFirstFrame && media_main_g_InjectDeliveredFirstFrame.load(std::memory_order_acquire)) {
                receivedFirstFrame = true;
                LogInfo("[Media] Inject delivery confirmed before monitor observed ring activity");
            }

            if (autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kStarting) {
                if (receivedFirstFrame) {
                    const auto transition = autoWgcHandoff.OnInjectFrame();
                    if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                        StopWgcCapturePipeline();
                        media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogInfo(
                            "[Media] Inject delivered while WGC fallback was warming; WGC standby stopped and "
                            "inject remains authoritative");
                    }
                } else {
                    bool wgcReady = false;
                    bool wgcFailed = false;
                    uint32_t observedFrames = 0;
                    if (auto capture = media_main_g_WgcCap.Read()) {
                        observedFrames = capture->GetCallbackFrameCount();
                        wgcFailed = !capture->IsCapturing() || capture->NeedsReset();
                        wgcReady = !wgcFailed && (config.video.useVFR ? HasStandbyWgcHandoffFrame()
                                                                      : observedFrames > autoWgcHandoffBaselineFrames);
                    } else {
                        wgcFailed = true;
                    }

                    if (wgcReady) {
                        const auto transition = autoWgcHandoff.OnWgcFirstFrame();
                        if (transition.action == ce::capture_handoff::Action::kCommitWgcStopInject) {
                            // Invalidate every cached/queued frame from the old
                            // inject/WGC lineage before the encoder observes the
                            // new active path. The CFR/audio clock itself is not
                            // restarted.
                            // Keep the standby capture's publication epoch. Its
                            // proven first frame already carries that identity;
                            // advancing here would discard the proof, clear the
                            // inject repeat cache, and create a handoff hole
                            // before any replacement-epoch frame exists.
                            media_main_g_RejectInjectFrames.store(true, std::memory_order_release);
                            media_main_g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                            QueuedFrame retainedVfrFrame;
                            const bool hasRetainedVfrFrame =
                                config.video.useVFR && TakeStandbyWgcHandoffFrame(retainedVfrFrame);
                            SetActiveScreenGrab(true);
                            SetInjectVideoCaptureRequestedState(false, "auto inject-to-WGC handoff committed");
                            if (hasRetainedVfrFrame) {
                                SubmitWgcQueuedFrame(std::move(retainedVfrFrame));
                            }
                            StopInjectCapturePipeline();
                            LogInfo(
                                "[Media] Active recording path switched to WGC after first-frame proof "
                                "(inputFrames=%u); inject stopped only after the replacement was delivering",
                                observedFrames);
                        }
                    } else if (wgcFailed) {
                        const auto transition = autoWgcHandoff.OnWgcFailure();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback stopped before first-frame proof; inject capture remains "
                                "active");
                        }
                    } else if (GetTickCount64() >= autoWgcHandoffDeadlineTick) {
                        const auto transition = autoWgcHandoff.OnWgcReadinessTimeout();
                        if (transition.action == ce::capture_handoff::Action::kStopWgcKeepInject) {
                            StopWgcCapturePipeline();
                            media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                            LogWarn(
                                "[Media] WGC fallback produced no frame within %llums; inject capture remains "
                                "active",
                                static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                        }
                    }
                }
            }

            if (!receivedFirstFrame && autoWgcHandoff.GetPhase() == ce::capture_handoff::Phase::kIdle &&
                isAutoCaptureConfig() && media_main_g_AutoWgcFallbackArmed.load(std::memory_order_acquire) && media_main_g_WgcCap) {
                DWORD elapsed = GetTickCount() - injectModeStartTime;
                const uint32_t activeSourcePid = media_main_g_pSharedMem ? media_main_g_pSharedMem->GetSourcePid() : 0;
                if (ce::capture_policy::ShouldTriggerAutoWgcFallback(
                        receivedFirstFrame, isAutoCaptureConfig(),
                        media_main_g_AutoWgcFallbackArmed.load(std::memory_order_acquire), media_main_g_WgcCap != nullptr, elapsed,
                        activeSourcePid)) {
                    LogInfo("[Media] No frames from inject mode after %lums - starting WGC standby handoff", elapsed);

                    const auto transition = autoWgcHandoff.Begin();
                    media_main_g_AutoWgcFallbackArmed.store(false, std::memory_order_release);
                    ClearStandbyWgcHandoffFrame();
                    media_main_g_RetainStandbyWgcFrameForHandoff.store(config.video.useVFR, std::memory_order_release);
                    if (transition.action == ce::capture_handoff::Action::kStartWgcKeepInject &&
                        StartWgcRecordingCapture(config)) {
                        autoWgcHandoffBaselineFrames = 0;
                        autoWgcHandoffDeadlineTick = GetTickCount64() + kAutoWgcHandoffReadyTimeoutMs;
                        LogInfo(
                            "[Media] WGC fallback session started; inject remains active pending first-frame "
                            "proof (timeout=%llums)",
                            static_cast<unsigned long long>(kAutoWgcHandoffReadyTimeoutMs));
                    } else {
                        media_main_g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
                        ClearStandbyWgcHandoffFrame();
                        autoWgcHandoff.OnWgcFailure();
                        media_main_g_RejectInjectFrames.store(false, std::memory_order_release);
                        LogWarn("[Media] WGC fallback failed to start; inject capture remains active");
                    }

                    injectModeStartTime = 0;
                }
            }

            if (!sharedTexturesCreated && media_main_g_pSharedMem->GetWidth() > 0 && media_main_g_pSharedMem->GetHeight() > 0) {
                if (media_main_g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                    sharedTexturesCreated = true;
                }
            }

            if (wIdx > localReadIdx + FRAME_RING_SIZE) {
                uint32_t newReadIdx = wIdx - FRAME_RING_SIZE;
                localReadIdx = newReadIdx;
            }

            if (localReadIdx < wIdx) {
                if (!receivedFirstFrame) {
                    receivedFirstFrame = true;
                    LogInfo("[Media] First frame detected (monitoring)");
                }
                localReadIdx = wIdx;
            }

            hasPendingInputs = false;
        } else {
            recordingStartTime = 0;
        }

        if (media_main_g_Recording && (media_main_g_FrameQueue.Size() > 0 || hasPendingInputs)) {
            Sleep(1);
        } else {
            Sleep(5);
        }
    }

    PublishMediaScreenGrabTarget(0, nullptr, false, "media process exit");
    StopRecording();

    if (auto capture = media_main_g_WgcCap.LockExclusive()) {
        capture->StopCapture();
    }
    if (d3dContext)
        d3dContext->Release();
    d3dDevice = nullptr;

    // Shutdown media engine BEFORE unmapping shared memory to avoid use-after-free
    // (VideoEncoder::CleanupResources accesses pSharedMem during destruction)
    if (mediaEngineReady && MediaEngine_Shutdown) {
        MediaEngine_Shutdown();
        mediaEngineReady = false;
    }
    MediaEngine_Unload();

    // Release every remaining metadata/resource lease while the cross-process
    // mapping is still valid. Normal recording stop already does this, but the
    // process-exit path also covers partial startup failures and shutdowns that
    // occur before a recording becomes active.
    media_main_g_FrameQueue.Clear();
    ClearStandbyWgcHandoffFrame();
    ResetLastQueuedFrameCache();

    if (media_main_g_InjectFrameReadyEvent) {
        CloseHandle(media_main_g_InjectFrameReadyEvent);
        media_main_g_InjectFrameReadyEvent = NULL;
    }
    if (media_main_g_InjectCaptureShutdownEvent) {
        CloseHandle(media_main_g_InjectCaptureShutdownEvent);
        media_main_g_InjectCaptureShutdownEvent = NULL;
    }

    if (media_main_g_pShmem)
        UnmapViewOfFile(media_main_g_pShmem);
    if (media_main_g_hMapShmem)
        CloseHandle(media_main_g_hMapShmem);

    if (media_main_g_pSharedMem)
        UnmapViewOfFile(media_main_g_pSharedMem);
    if (media_main_g_hMapFile)
        CloseHandle(media_main_g_hMapFile);

    LogInfo("[Media] Process exiting");
    return 0;
}
