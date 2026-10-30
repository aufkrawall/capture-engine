#include "media_main_internal.h"

std::string MediaProcessSession::refreshActiveConfig(bool forceReload, HWND targetWindow, uint32_t confirmedPid,
                                                     const std::string& confirmedProcessName,
                                                     const std::string& confirmedProcessPath) {
    uint32_t sourcePid = 0;
    std::string processName;
    std::string processPath;
    if (MediaState().pSharedMem) {
        sourcePid = MediaState().pSharedMem->GetSourcePid();
        if (sourcePid != 0) {
            const ce::process::ProcessIdentityResult identity = GetProcessIdentityFromPID(sourcePid);
            processName = identity.imageName;
            processPath = identity.imagePath;
            if (processName.empty() && sourcePid == activeConfigSourcePid) {
                processName = activeConfigProcessName;
                processPath = activeConfigProcessPath;
            }
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
            if (sourcePid == confirmedPid && !confirmedProcessName.empty()) {
                processName = confirmedProcessName;
                processPath = confirmedProcessPath;
            } else {
                const ce::process::ProcessIdentityResult identity = GetProcessIdentityFromPID(sourcePid);
                processName = identity.imageName;
                processPath = identity.imagePath;
            }
            if (processName.empty() && sourcePid == activeConfigSourcePid) {
                processName = activeConfigProcessName;
                processPath = activeConfigProcessPath;
            }
        }
    }
    if (sourcePid == 0 && MediaState().recording && activeConfigSourcePid != 0) {
        sourcePid = activeConfigSourcePid;
        processName = activeConfigProcessName;
        processPath = activeConfigProcessPath;
    }

    if (!forceReload && sourcePid == activeConfigSourcePid && processName == activeConfigProcessName &&
        processPath == activeConfigProcessPath) {
        return processName;
    }

    AppConfig resolvedConfig;
    LoadConfig(configPath, resolvedConfig);
    if (!processName.empty()) {
        LoadConfig(configPath, resolvedConfig, processName, nullptr, processPath);
    }
    // Normalize runtime-only sync state before comparing with the active config. Otherwise a
    // config.ini reload with no real media changes would compare file defaults (usually 0 ms)
    // against the active auto-detected render latency and reload unnecessarily.
    ApplyAutoDetectedRenderLatencyToConfig(resolvedConfig);

    const bool mediaConfigChanged = !MediaEngineConfigEquals(config, resolvedConfig);

    config = std::move(resolvedConfig);
    if (roleContext.ownsProcess)
        Log_SetLevel(config.logLevel);
    activeConfigSourcePid = sourcePid;
    activeConfigProcessName = processName;
    activeConfigProcessPath = processPath;

    if (MediaState().recording && IsActiveScreenGrab())
        PublishMediaScreenGrabTarget(activeConfigSourcePid, d3dDevice, true, "active profile refresh");

    ApplyMediaPrioritySettings(config);
    if (d3dDevice) {
        ApplyMediaGpuSchedulingPriorityForDevice(config, d3dDevice);
    } else {
        ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
    }
    if (auto capture = MediaState().wgcCap.Read()) {
        applyWgcOptions(capture.get());
        capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
    }
    if (mediaEngineReady) {
        ApplyMediaEngineLogging(IsDebugLoggingEnabled(config.logLevel));
        if (forceReload || mediaConfigChanged) {
            ce::mediaengine_api::MediaEngine_ReloadConfig(MediaState().engine, &config);
        }
        if (MediaState().pSharedMem || MediaState().pShmem) {
            ce::mediaengine_api::MediaEngine_SetSharedMem(MediaState().engine, MediaState().pSharedMem.get(),
                                                          MediaState().pShmem);
        }
    }
    return processName;
}
