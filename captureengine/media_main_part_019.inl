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
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);

    if (g_pSharedMem) {
        ResetRuntimeDiagnostics(g_pSharedMem);
        g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
    g_CfrDrainStopQpc.store(0, std::memory_order_release);
    g_RecordingUsesVfr.store(false, std::memory_order_release);
    SetActiveScreenGrab(false);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    LogInfo("[Media] Recording stopped");
    timeEndPeriod(1);
}

int MediaProcessMain(const AppConfig& initialConfig) {
    AppConfig config = initialConfig;
    g_RecordingManifestLogPath =
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
        if (mediaEngineReady && g_AudioOnly) {
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
        if (g_AudioOnly && MediaEngine_SetAudioOnly) {
            MediaEngine_SetAudioOnly(true);
        }
        // Auto-detect the render-domain delay before Init. The product-safe path is audio-only:
        // no calibration window, no WGC/DX stimulus, and no config.ini delay writeback. Apply the
        // result (or explicit low-confidence reason) before the media engine snapshots config.
        if (!g_Recording.load(std::memory_order_acquire)) {
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

        if (g_pSharedMem || g_pShmem) {
            MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);
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

    for (int retry = 0; retry < 10 && !g_pSharedMem; retry++) {
        HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDiscovery) {
            DiscoveryInfo* pDiscovery =
                (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (ValidateDiscoveryInfo(pDiscovery) && pDiscovery->GetInjectPid() != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDiscovery->GetInjectPid());

                g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (g_hMapFile) {
                    g_pSharedMem = (SharedMemoryLayout*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                      sizeof(SharedMemoryLayout));

                    if (g_pSharedMem && ValidateSharedMemory(g_pSharedMem) && g_pSharedMem->GetHostPID() != 0) {
                        LogInfo("[Media] Connected via discovery (inject PID: %u, ABI: 0x%08X)",
                                pDiscovery->injectPid.load(), SHARED_MEMORY_ABI_SIGNATURE);
                        UnmapViewOfFile(pDiscovery);
                        CloseHandle(hDiscovery);
                        break;
                    }

                    if (g_pSharedMem) {
                        LogError(
                            "[Media] Rejected shared memory header: magic=0x%08X version=%u size=%u abi=0x%08X "
                            "expected=(0x%08X,%u,%zu,0x%08X)",
                            g_pSharedMem->GetMagic(), g_pSharedMem->GetVersion(),
                            g_pSharedMem->structSize.load(std::memory_order_acquire),
                            g_pSharedMem->abiSignature.load(std::memory_order_acquire), SHARED_MEMORY_MAGIC,
                            SHARED_MEMORY_VERSION, sizeof(SharedMemoryLayout), SHARED_MEMORY_ABI_SIGNATURE);
                    }

                    if (g_pSharedMem) {
                        UnmapViewOfFile(g_pSharedMem);
                        g_pSharedMem = nullptr;
                    }
                    CloseHandle(g_hMapFile);
                    g_hMapFile = NULL;
                }
                UnmapViewOfFile(pDiscovery);
            }
            CloseHandle(hDiscovery);
        }

        if (!g_pSharedMem) {
            Sleep(50);
        }
    }

    if (g_pSharedMem) {
        if (g_pSharedMem->GetShmemMappingCreated()) {
            wchar_t shmemName[64];
            GenerateShmemName(shmemName, 64, g_pSharedMem->GetHostPID());
            g_hMapShmem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmemName);
            if (g_hMapShmem) {
                size_t mapSize = g_pSharedMem->GetShmemMappingSize();
                g_pShmem = (ShmemBuffer*)MapViewOfFile(g_hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
                if (g_pShmem) {
                    LogInfo("[Media] Connected to separate Shmem mapping '%ls' (mapped %zu bytes)", shmemName, mapSize);
                }
            }
        }

        if (mediaEngineReady) {
            MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);
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
        if (g_pSharedMem) {
            sourcePid = g_pSharedMem->GetSourcePid();
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
        if (sourcePid == 0 && g_Recording && activeConfigSourcePid != 0) {
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

        if (g_Recording && IsActiveScreenGrab())
            PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice, true, "active profile refresh");

        ApplyMediaPrioritySettings(config);
        if (d3dDevice) {
            ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
        } else {
            ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        }
        if (auto capture = g_WgcCap.Read()) {
            applyWgcOptions(capture.get());
            capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
        }
        if (mediaEngineReady) {
            MediaEngine_SetLogCallback(IsDebugLoggingEnabled(config.logLevel) ? MediaLogCallback : nullptr);
            if (forceReload || mediaConfigChanged) {
                MediaEngine_ReloadConfig(&config);
            }
            if (g_pSharedMem || g_pShmem) {
                MediaEngine_SetSharedMem(g_pSharedMem, g_pShmem);
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
            const auto existingCapture = g_WgcCap.Read();
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

        const auto capture = g_WgcCap.Read();
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
            const auto existingCapture = g_WgcCap.Read();
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
