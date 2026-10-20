#include "media_main_internal.h"

#include "common/capture/capture_retarget_policy.h"
#include "common/config/live_stream_config.h"

int MediaProcessSession::Run(const AppConfig& initialConfig) {
    config = initialConfig;
    MediaState().recordingManifestLogPath =
        IsAnyLoggingEnabled(initialConfig.logLevel) ? initialConfig.logFilePath : std::string{};
    Log_SetLevel(config.logLevel);
    SetConsoleCtrlHandler(MediaConsoleHandler, TRUE);

    const int initStatus = Init();
    if (initStatus != 0) {
        return initStatus;
    }
    Loop();
    Shutdown();
    // The exit code is the finalization outcome (common/ipc/media_finalization.h): 0 when no recording
    // finalized, a coded result otherwise. It reaches the spawner even when no result slot was
    // inherited and whatever the log level, and a crash before this point leaves an uncoded code.
    const uint32_t exitCode =
        ce::media_result::EncodeExitCode(MediaState().lastFinalization.status, MediaState().lastFinalization.flags);
    LogInfo("[MediaResult] Exiting with code 0x%08X (result=%s finalizations=%u)", exitCode,
            ce::media_result::StatusName(MediaState().lastFinalization.status),
            MediaState().lastFinalization.finalizationCount);
    return static_cast<int>(exitCode);
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

    // Map the inherited session A/V latency channel before the media engine loads: the
    // render->loopback probe runs inside ensureMediaEngineReady() below, and this process is
    // disposable, so without the channel it would re-measure (about a second) on every recording start.
    latencyChannel = ce::av_sync::MapInheritedLatencyChannel(ParseInheritedLatencyChannelHandle());

    // The per-child result slot: how this process reports its finalization outcome and output path to
    // whoever spawned it, independent of the log level. Null when none was inherited; the exit code
    // then carries the outcome alone.
    MediaState().resultSlot = ce::media_result::MapInheritedSlot(ParseInheritedMediaResultHandle());
    LogInfo("[MediaResult] Result slot %s", MediaState().resultSlot ? "attached" : "absent; exit code only");

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

    for (int retry = 0; retry < 10 && !MediaState().pSharedMem; retry++) {
        HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDiscovery) {
            DiscoveryInfo* pDiscovery =
                (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (ValidateDiscoveryInfo(pDiscovery) && pDiscovery->GetInjectPid() != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDiscovery->GetInjectPid());

                MediaState().hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (MediaState().hMapFile) {
                    MediaState().pSharedMem = (SharedMemoryLayout*)MapViewOfFile(MediaState().hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                      sizeof(SharedMemoryLayout));

                    if (MediaState().pSharedMem && ValidateSharedMemory(MediaState().pSharedMem) && MediaState().pSharedMem->GetHostPID() != 0) {
                        LogInfo("[Media] Connected via discovery (inject PID: %u, ABI: 0x%08X)",
                                pDiscovery->injectPid.load(), SHARED_MEMORY_ABI_SIGNATURE);
                        UnmapViewOfFile(pDiscovery);
                        CloseHandle(hDiscovery);
                        break;
                    }

                    if (MediaState().pSharedMem) {
                        LogError(
                            "[Media] Rejected shared memory header: magic=0x%08X version=%u size=%u abi=0x%08X "
                            "expected=(0x%08X,%u,%zu,0x%08X)",
                            MediaState().pSharedMem->GetMagic(), MediaState().pSharedMem->GetVersion(),
                            MediaState().pSharedMem->structSize.load(std::memory_order_acquire),
                            MediaState().pSharedMem->abiSignature.load(std::memory_order_acquire), SHARED_MEMORY_MAGIC,
                            SHARED_MEMORY_VERSION, sizeof(SharedMemoryLayout), SHARED_MEMORY_ABI_SIGNATURE);
                    }

                    if (MediaState().pSharedMem) {
                        UnmapViewOfFile(MediaState().pSharedMem);
                        MediaState().pSharedMem = nullptr;
                    }
                    CloseHandle(MediaState().hMapFile);
                    MediaState().hMapFile = NULL;
                }
                UnmapViewOfFile(pDiscovery);
            }
            CloseHandle(hDiscovery);
        }

        if (!MediaState().pSharedMem) {
            Sleep(50);
        }
    }

    if (MediaState().pSharedMem) {
        if (MediaState().pSharedMem->GetShmemMappingCreated()) {
            wchar_t shmemName[64];
            GenerateShmemName(shmemName, 64, MediaState().pSharedMem->GetHostPID());
            MediaState().hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
            if (MediaState().hMapShmem) {
                size_t mapSize = MediaState().pSharedMem->GetShmemMappingSize();
                MediaState().pShmem = (ShmemBuffer*)MapViewOfFile(MediaState().hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
                if (MediaState().pShmem) {
                    LogInfo("[Media] Connected to separate Shmem mapping '%ls' (mapped %zu bytes)", shmemName, mapSize);
                }
            }
        }

        if (mediaEngineReady) {
            MediaEngine_SetSharedMem(MediaState().pSharedMem, MediaState().pShmem);
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
    if (mediaEngineReady && MediaState().audioOnly) {
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
    // Hand the session latency channel over before the probe below can run. Re-applied on every
    // engine (re)load because MediaEngine_Unload drops the DLL's copy of the pointer.
    if (MediaEngine_SetRenderLatencyChannel) {
        MediaEngine_SetRenderLatencyChannel(latencyChannel);
    }
    // Propagate audio-only flag to MediaEngine before Init
    if (MediaState().audioOnly && MediaEngine_SetAudioOnly) {
        MediaEngine_SetAudioOnly(true);
    }
    // Auto-detect the render-domain delay before Init. The product-safe path is audio-only:
    // no calibration window, no WGC/DX stimulus, and no config.ini delay writeback. Apply the
    // result (or explicit low-confidence reason) before the media engine snapshots config.
    if (!MediaState().recording.load(std::memory_order_acquire)) {
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

    if (MediaState().pSharedMem || MediaState().pShmem) {
        MediaEngine_SetSharedMem(MediaState().pSharedMem, MediaState().pShmem);
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
    // The helper's one session: the process default for every unbound thread, bound on this one too. Static, so
    // it lives as long as the process did when these were globals (threads may still be running at exit).
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - constructed once, before any media thread starts
    static MediaSessionState processSession;
    processSession.recordingId = g_RecordingId;
    static MediaProcessDefaultScope processDefault(processSession);
    MediaSessionBinding binding(processSession);
    return MediaProcessSession().Run(initialConfig);
}
// Re-assert latched source-loss truth into the health registers before
// finalization: the encoder thread's shutdown publications run inside
// StopRecording and would otherwise erase it, letting the manifest and the
// completion notification claim a clean save for a recording whose source died.
static void PublishLatchedSourceLossBeforeFinalization() {
    ce::capture_retarget::PublishLatchedSourceLossHealth(
        MediaState().sourceLossHealthFlags, MediaState().recordingHealthFlags,
        MediaState().pSharedMem ? &MediaState().pSharedMem->runtimeState.recordingHealthFlags : nullptr);
}

bool StartRecording(const AppConfig& config) {
    if (MediaState().recording)
        return true;

    MediaState().liveStreamRecording.store(false, std::memory_order_release);
    MediaState().privacyFailClosedStopRequested.store(false, std::memory_order_release);
    ResetRecordingHealthPublication();
    ce::capture_retarget::ResetSourceLossHealth(MediaState().sourceLossHealthFlags);
    LogInfo("[Media] Starting recording...");

    timeBeginPeriod(1);

    // A prior interrupted session must never leave hook-side video publication
    // armed. The selected live path below explicitly enables it only for inject.
    SetInjectVideoCaptureRequestedState(false, "recording start reset");

    if (MediaState().audioOnly) {
        LogInfo("[Media] Audio-only recording mode - skipping video capture");

        // Clear any stale shared memory state
        if (MediaState().pSharedMem) {
            StoreRelease(MediaState().pSharedMem->runtimeState.cmdStartRecording, false);
            StoreRelease(MediaState().pSharedMem->runtimeState.cmdStopRecording, false);
            StoreRelease(MediaState().pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::None));
            SetRecordingVisibleState(false);
        }

        if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
            LogError("[Media] Failed to start MediaEngine audio-only recording");
            SetRecordingVisibleState(false);
            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                         "MediaEngine audio-only initialization");
            timeEndPeriod(1);
            MediaState().audioOnly = false;
            return false;
        }

        MediaState().recording = true;
        SetRecordingVisibleState(true);
        SetCapturePipelinePhase(CapturePipelinePhase::kLive);

        LogInfo("[Media] Audio-only recording active");
        return true;
    }

    MediaState().liveStreamRecording.store(
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

    if (useScreenGrab && !MediaState().wgcCap) {
        LogError("[Media] WGC capture requested but no WGC target is available");
        SetActiveScreenGrab(false);
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "WGC target unavailable");
        timeEndPeriod(1);
        return false;
    }

    if (!useScreenGrab && MediaState().pSharedMem) {
        const uint32_t writeIndex = MediaState().pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
        const uint32_t ingestIndex = MediaState().pSharedMem->frameRing.ingestIndex.load(std::memory_order_acquire);
        const uint32_t readIndex = MediaState().pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
        if (!IsFrameRingWindowValid(writeIndex, readIndex) ||
            static_cast<uint32_t>(ingestIndex - readIndex) > static_cast<uint32_t>(writeIndex - readIndex)) {
            LogError(
                "[Media] Inject recording rejected corrupt shared frame ring "
                "(write=%u ingest=%u read=%u distance=%u version=%u ABI=0x%08X)",
                writeIndex, ingestIndex, readIndex, static_cast<uint32_t>(writeIndex - readIndex),
                MediaState().pSharedMem->GetVersion(), MediaState().pSharedMem->abiSignature.load(std::memory_order_acquire));
            StoreRelease(MediaState().pSharedMem->runtimeState.recordingFailureCode,
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
    if (MediaState().pSharedMem) {
        StoreRelease(MediaState().pSharedMem->runtimeState.cmdStartRecording, false);
        StoreRelease(MediaState().pSharedMem->runtimeState.cmdStopRecording, false);
        StoreRelease(MediaState().pSharedMem->runtimeState.recordingFailureCode,
                     static_cast<uint32_t>(RecordingFailureCode::None));
        SetRecordingVisibleState(false);
    }

    // Reset inject session state so main loop re-initializes on new recording
    MediaState().injectSessionReset.store(true, std::memory_order_release);

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    MediaState().frameQueue.Clear();
    ResetLastQueuedFrameCache();
    ResetInjectFrameRingToLatest("recording start");
    MediaState().injectDeliveredFirstFrame.store(false, std::memory_order_release);
    MediaState().rejectInjectFrames.store(false, std::memory_order_release);
    MediaState().isEncoderBottlenecked.store(false, std::memory_order_relaxed);
    MediaState().injectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
    MediaState().injectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
    MediaState().injectDeferredFrames.store(0, std::memory_order_relaxed);
    MediaState().activePathMismatchFramesDiscarded.store(0, std::memory_order_relaxed);

    if (MediaState().pSharedMem) {
        ResetRuntimeDiagnostics(MediaState().pSharedMem);
        MediaState().pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    EnsureInjectCaptureEvents();
    if (MediaState().injectCaptureShutdownEvent) {
        ResetEvent(MediaState().injectCaptureShutdownEvent);
    }
    if (MediaState().injectFrameReadyEvent) {
        ResetEvent(MediaState().injectFrameReadyEvent);
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

    MediaState().recording = true;
    MediaState().recordingEverStarted.store(true, std::memory_order_release);
    MediaState().liveStreamRecording.store(
        ce::live_stream::IsLiveStreamTarget(config.video.outputDir), std::memory_order_release);
    MediaState().encoderRunning = true;
    MediaState().recordingUsesVfr.store(config.video.useVFR, std::memory_order_release);
    MediaState().drainOutstandingCfrTicks.store(false, std::memory_order_release);
    MediaState().cfrDrainStopQpc.store(0, std::memory_order_release);

    // Recording-lifetime config snapshot (see StartWgcRecordingCapture): the
    // main thread may reassign `config` mid-recording; encoder-thread settings
    // are fixed per session by design, so it reads an owned copy.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        MediaSessionState* session = &MediaState();
        MediaState().encoderThread = std::thread([configSnapshot, session]() {
            MediaSessionBinding binding(*session);
            EncoderThreadFunc(*configSnapshot);
        });
    }

    if (useScreenGrab && MediaState().wgcCap) {
        if (!StartWgcRecordingCapture(config)) {
            LogError("[Media] Failed to start WGC capture");
            MediaState().encoderRunning = false;
            JoinThreadWithTimeout(MediaState().encoderThread, 10000, "encoder");
            MediaState().recording = false;
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            MediaEngine_StopRecording(true);
            SetActiveScreenGrab(false);
            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "WGC capture initialization");
            timeEndPeriod(1);
            return false;
        }
        LogInfo("[Media] Active recording path: %s bounded pull-drain CFR (%d fps output)",
                MediaState().wgcCap->IsUsingDesktopDuplication() ? "DXGI-duplication" : "WGC", config.video.fps);
    } else if (!useScreenGrab) {
        if (config.captureMethod == "auto" && MediaState().wgcCap && MediaState().autoWgcFallbackArmed.load(std::memory_order_acquire)) {
            LogInfo("[Media] Active recording path: inject shared-memory capture (WGC auto-fallback armed)");
        } else {
            LogInfo("[Media] Active recording path: inject shared-memory capture");
        }
        ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        MediaState().injectCaptureShutdown = false;
        EnsureInjectCaptureEvents();
        // Recording-lifetime config snapshot (see StartWgcRecordingCapture).
        {
            auto configSnapshot = std::make_shared<const AppConfig>(config);
            MediaSessionState* session = &MediaState();
            MediaState().injectCaptureThread = std::thread([configSnapshot, session]() {
                MediaSessionBinding binding(*session);
                InjectCaptureThreadFunc(*configSnapshot);
            });
        }
    }

    LogInfo("[Media] Recording warmup armed");
    return true;
}
void StopRecording() {
    if (MediaState().pSharedMem) {
        MediaState().pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
    }
    if (!MediaState().recording)
        return;

    LogInfo("[Media] Stopping recording...");

    SetInjectVideoCaptureRequestedState(false, "recording stop");

    // Audio-only: skip all video/capture cleanup
    if (MediaState().audioOnly) {
        MediaState().recording = false;
        SetRecordingVisibleState(false);
        SetCapturePipelinePhase(CapturePipelinePhase::kStopping);

        const bool outputSaved = MediaEngine_StopRecording(false);
        PublishLatchedSourceLossBeforeFinalization();
        CompleteRecordingFinalization(false, outputSaved);

        if (MediaState().pSharedMem) {
            ResetRuntimeDiagnostics(MediaState().pSharedMem);
            MediaState().pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
            MediaState().pSharedMem->throttleCapture.store(false, std::memory_order_release);
        }

        SetActiveScreenGrab(false);
        MediaState().audioOnly = false;
        timeEndPeriod(1);
        LogInfo("[Media] Audio-only recording stopped");
        return;
    }

    const bool wasActiveScreenGrab = IsActiveScreenGrab();
    const bool recordingUsesVfr = MediaState().recordingUsesVfr.load(std::memory_order_acquire);
    MediaState().recording = false;
    const CapturePipelinePhase stopTransition = BeginCapturePipelineStop();
    const bool cancelBeforeLive = stopTransition == CapturePipelinePhase::kCancelling;
    const bool drainOutstandingCfrTicks =
        !cancelBeforeLive &&
        ce::capture_policy::ShouldDrainOutstandingCfrTicksAtStop(wasActiveScreenGrab, recordingUsesVfr);
    LogInfo("[RecordingLifecycle] Stop accepted as %s (path=%s liveFrames=%u)",
            cancelBeforeLive ? "pre-live cancellation" : "live finalization",
            wasActiveScreenGrab ? "WGC" : "inject",
            MediaState().pSharedMem ? MediaState().pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_acquire) : 0u);
    int64_t drainStopQpc = 0;
    if (drainOutstandingCfrTicks) {
        LARGE_INTEGER stopQpc;
        QueryPerformanceCounter(&stopQpc);
        drainStopQpc = stopQpc.QuadPart;
    }

    SetCaptureRequestedState(false);
    SetRecordingVisibleState(false);
    MediaState().cfrDrainStopQpc.store(drainStopQpc, std::memory_order_release);
    MediaState().drainOutstandingCfrTicks.store(drainOutstandingCfrTicks && drainStopQpc > 0, std::memory_order_release);
    if (wasActiveScreenGrab) {
        MediaState().encoderRunning = false;
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
        MediaState().encoderRunning = false;
    }

    MediaState().injectDeliveredFirstFrame.store(false, std::memory_order_release);
    MediaState().rejectInjectFrames.store(false, std::memory_order_release);
    MediaState().autoWgcFallbackArmed.store(false, std::memory_order_release);

    const bool encoderJoined = JoinThreadWithTimeout(MediaState().encoderThread, 60000, "encoder");

    if (wasActiveScreenGrab && !encoderJoined) {
        LogWarn("[Media] WGC encoder join timed out after exact-stop shutdown");
    }

    MediaState().frameQueue.Clear();
    ResetLastQueuedFrameCache();
    // The encoder polls its source-read markers only between its own submits;
    // once its thread joined, no submit can race the context, so the last ones
    // complete here. A still-running encoder keeps them (the drain then keeps
    // the leases held rather than racing NVENC on the immediate context).
    if (encoderJoined && MediaEngine_FinishInjectSourceReads && !MediaEngine_FinishInjectSourceReads(5000))
        LogWarn("[Inject SourceRead] Encoder still has GPU reads pending at teardown");
    if (!MediaState().injectSourceReadReleases.Drain(5000)) {
        LogError("[Inject SourceRead] Teardown timed out with %zu GPU read(s) pending; ring leases remain held",
                 MediaState().injectSourceReadReleases.Pending());
    }
    LARGE_INTEGER sourceReadFrequency{};
    QueryPerformanceFrequency(&sourceReadFrequency);
    LogInjectSourceReadSummary(sourceReadFrequency.QuadPart);
    ResetInjectFrameRingToLatest("recording stop");

    if (MediaState().pSharedMem) {
        // Recording has stopped, so zero-copy encoder textures must not stay
        // live. Clear the handshake before stopping the encoder so the DLL
        // tears down all preserved D3D11/KMT resources immediately.
        MediaState().pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        MediaState().pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        MediaState().pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);

    }

    const bool outputSaved = MediaEngine_StopRecording(cancelBeforeLive);
    PublishLatchedSourceLossBeforeFinalization();
    CompleteRecordingFinalization(cancelBeforeLive, outputSaved);
    if (MediaEngine_ReleaseEncoderTextures) {
        MediaEngine_ReleaseEncoderTextures();
    }
    // Reset WGC-specific 10-bit hint so inject-mode recordings don't inherit it.
    if (MediaEngine_SetSourcePrefers10Bit) {
        MediaEngine_SetSourcePrefers10Bit(false);
    }
    MediaState().isEncoderBottlenecked.store(false, std::memory_order_relaxed);

    if (MediaState().pSharedMem) {
        ResetRuntimeDiagnostics(MediaState().pSharedMem);
        MediaState().pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    MediaState().drainOutstandingCfrTicks.store(false, std::memory_order_release);
    MediaState().cfrDrainStopQpc.store(0, std::memory_order_release);
    MediaState().recordingUsesVfr.store(false, std::memory_order_release);
    SetActiveScreenGrab(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LogInfo("[Media] Recording stopped");
    timeEndPeriod(1);
}
