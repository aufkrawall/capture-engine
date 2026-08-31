#include "media_main_internal.h"

#include "../common/live_stream_config.h"

int MediaProcessSession::Run(const AppConfig& initialConfig) {
    config = initialConfig;
    media_main_g_RecordingManifestLogPath =
        IsAnyLoggingEnabled(initialConfig.logLevel) ? initialConfig.logFilePath : std::string{};
    Log_SetLevel(config.logLevel);
    SetConsoleCtrlHandler(MediaConsoleHandler, TRUE);

    const int initStatus = Init();
    if (initStatus != 0) {
        return initStatus;
    }
    Loop();
    Shutdown();
    return 0;
}

int MediaProcessSession::Init() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    configPath = GetLocalConfigPath();
    // Directory for the per-device render-endpoint latency cache (next to the config).
    mediaCacheDir = configPath;
    {
        const size_t slash = mediaCacheDir.find_last_of("\\/");
        mediaCacheDir = (slash != std::string::npos) ? mediaCacheDir.substr(0, slash) : std::string();
    }

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

    QueryPerformanceFrequency(&qpcFreq);



    return 0;
}

void MediaProcessSession::unloadMediaEngineIdle() {
    if (mediaEngineReady && MediaEngine_Shutdown) {
        MediaEngine_Shutdown();
        mediaEngineReady = false;
    }
    MediaEngine_Unload();
    LogInfo("[Media] MediaEngine unloaded for idle state");
}


bool MediaProcessSession::ensureMediaEngineReady() {
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
}


bool MediaProcessSession::isExplicitInjectConfig() {
    return IsInjectCaptureMethod(config.captureMethod);
}


bool MediaProcessSession::isExplicitWgcConfig() {
    return IsWgcCaptureMethod(config.captureMethod);
}


bool MediaProcessSession::isExplicitDxgiDupConfig() {
    return IsDxgiDupCaptureMethod(config.captureMethod);
}


bool MediaProcessSession::isExplicitScreenGrabConfig() {
    return IsScreenGrabCaptureMethod(config.captureMethod);
}


bool MediaProcessSession::isAutoCaptureConfig() {
    return IsAutoCaptureMethod(config.captureMethod);
}


void MediaProcessSession::setWgcPreferenceAfterFailure() {
    SetPreferredScreenGrab(isExplicitScreenGrabConfig() || isAutoCaptureConfig());
}


bool MediaProcessSession::isInjectCaptureTarget(const std::string& processName) {
    if (const ApplicationProfile* profile = FindApplicationProfileForProcess(config, processName)) {
        return profile->resolvedVideoCapture == ApplicationVideoCapture::kInject;
    }
    const bool gameWhitelistMatched =
        !processName.empty() && MatchesProcessEntries(config.gameWhitelist, processName);
    return ce::capture_policy::ShouldUseInjectCaptureForAutoTarget(isExplicitInjectConfig(), isAutoCaptureConfig(),
                                                                   gameWhitelistMatched);
}


std::string MediaProcessSession::resolveSourceProcessName(uint32_t sourcePid, const std::string& knownName) {
    if (!knownName.empty() && knownName != "unknown") {
        return knownName;
    }
    if (sourcePid == 0) {
        return std::string{};
    }
    std::string resolvedName = GetProcessNameFromPID(sourcePid);
    return resolvedName == "unknown" ? std::string{} : resolvedName;
}


bool MediaProcessSession::isInjectCaptureTargetForSource(uint32_t sourcePid, const std::string& knownName) {
    return isInjectCaptureTarget(resolveSourceProcessName(sourcePid, knownName));
}


void MediaProcessSession::applyWgcOptions(WGCCapture* capture) {
    if (!capture) {
        return;
    }
    capture->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
    capture->SetSameDeviceCapture(config.wgcSameDeviceCapture);
    capture->SetAllowLossyBgra8Pool(config.wgcAllowLossyBgra8Pool);
    capture->SetVideoMemoryReservationMode(config.wgcVideoMemoryReservation);
}


int MediaProcessMain(const AppConfig& initialConfig) {
    return MediaProcessSession().Run(initialConfig);
}
bool StartRecording(const AppConfig& config) {
    if (media_main_g_Recording)
        return true;

    media_main_g_LiveStreamRecording.store(false, std::memory_order_release);
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

    media_main_g_LiveStreamRecording.store(
        ce::live_stream::IsLiveStreamTarget(config.video.outputDir), std::memory_order_release);

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
    media_main_g_LiveStreamRecording.store(
        ce::live_stream::IsLiveStreamTarget(config.video.outputDir), std::memory_order_release);
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
