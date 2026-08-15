#include "ipc.h"
#include <cstdio>
#include <cstring>
#include <new>
#include "../common/logging.h"

IPCManager::IPCManager(const AppConfig& config)
    : config(config),
      hMapFile(NULL),
      pSharedMem(NULL),
      hMapShmem(NULL),
      pShmem(NULL) {}

IPCManager::~IPCManager() {
    if (pShmem)
        UnmapViewOfFile(pShmem);
    if (hMapShmem)
        CloseHandle(hMapShmem);

    if (pSharedMem)
        UnmapViewOfFile(pSharedMem);
    if (hMapFile)
        CloseHandle(hMapFile);
}

#include <sddl.h>

bool IPCManager::Init() {
    // Host creates the shared memory with PID-based name
    wchar_t sharedMemName[64];
    GenerateSharedMemName(sharedMemName, 64, GetCurrentProcessId());

    // Create Security Descriptor for Low Integrity access (allows sandboxed
    // games/browsers) D: DACL (A;;GA;;;SY) - System: Generic All (A;;GA;;;BA) -
    // Built-in Admins: Generic All (A;;GA;;;IU) - Interactive User: Generic All
    // (A;;GA;;;AC) - All Application Packages (UWP): Generic All
    // S: SACL
    // (ML;;NW;;;LW) - Mandatory Label: No Write Up, Low Integrity Level
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    const wchar_t* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;AC)S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        OutputDebugStringA("Failed to create security descriptor from SDDL.");
        // Fallback to default (might fail for low integrity games)
        sa.lpSecurityDescriptor = NULL;
    }

    hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE,                  // Use paging file
                                  sa.lpSecurityDescriptor ? &sa : NULL,  // Custom security
                                  PAGE_READWRITE,                        // Read/write access
                                  0,                                     // Maximum object size (high-order DWORD)
                                  sizeof(SharedMemoryLayout),            // Maximum object size (low-order DWORD)
                                  sharedMemName);                        // Name of mapping object
    const DWORD mappingCreateError = GetLastError();

    if (sa.lpSecurityDescriptor) {
        LocalFree(sa.lpSecurityDescriptor);
    }

    if (hMapFile == NULL) {
        OutputDebugStringA("Could not create file mapping object.");
        return false;
    }
    if (mappingCreateError == ERROR_ALREADY_EXISTS) {
        LogError("[IPC] Refusing pre-existing shared memory mapping '%ls'", sharedMemName);
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }

    pSharedMem = (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));
    if (pSharedMem == NULL) {
        OutputDebugStringA("Could not map view of file.");
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }

    // Initialize shared memory using placement new to properly construct all
    // std::atomic members (ZeroMemory would bypass constructors — UB in C++).
    // On Windows, CreateFileMapping with INVALID_HANDLE_VALUE returns
    // OS-zero-initialized pages, so this is equivalent to ZeroMemory but correct.
    new (pSharedMem) SharedMemoryLayout();

    // CRITICAL: Initialize with proper ordering (version first, magic last as
    // signal)
    pSharedMem->structSize.store(sizeof(SharedMemoryLayout), std::memory_order_relaxed);
    pSharedMem->abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_relaxed);
    pSharedMem->SetVersion(SHARED_MEMORY_VERSION);

    pSharedMem->SetHostPID(GetCurrentProcessId());

    // Create separate Shmem mapping for large buffer
    wchar_t shmemName[64];
    GenerateShmemName(shmemName, 64, GetCurrentProcessId());

    // Host always creates mapping large enough for 4K
    size_t maxMappingSize = ShmemBuffer::CalculateSize(3840, 2160);

    hMapShmem = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, maxMappingSize, shmemName);

    if (hMapShmem) {
        pShmem = (ShmemBuffer*)MapViewOfFile(hMapShmem, FILE_MAP_ALL_ACCESS, 0, 0, maxMappingSize);
        if (pShmem) {
            new (pShmem) ShmemBuffer();  // Properly construct std::atomic members
            pSharedMem->SetShmemMappingCreated(true);
            pSharedMem->SetShmemMappingSize(maxMappingSize);
        }
    }

    // Set initial config
    UpdateConfig(config);
    pSharedMem->SetMagic(SHARED_MEMORY_MAGIC);  // Write last as the publication signal

    return true;
}

void IPCManager::UpdateConfig(const AppConfig& newConfig) {
    if (!pSharedMem)
        return;
    pSharedMem->BeginWriteOverlayConfig();
    pSharedMem->overlayConfig = newConfig.overlay;
    pSharedMem->EndWriteOverlayConfig();
    pSharedMem->SetDebugLogging(IsDebugLoggingEnabled(newConfig.logLevel));
    pSharedMem->SetLogLevel(newConfig.logLevel);

    // Copy log file path for hook logging
    strncpy(pSharedMem->logFilePath, newConfig.logFilePath.c_str(), sizeof(pSharedMem->logFilePath) - 1);
    pSharedMem->logFilePath[sizeof(pSharedMem->logFilePath) - 1] = '\0';

    // Copy priority settings for hook/encoder
    pSharedMem->SetGpuPriority(newConfig.video.gpuPriority);
    // Convert string to int: low=0, normal=1, high=2
    if (newConfig.copyQueuePriority == "low")
        pSharedMem->SetCopyQueuePriority(0);
    else if (newConfig.copyQueuePriority == "high")
        pSharedMem->SetCopyQueuePriority(2);
    else
        pSharedMem->SetCopyQueuePriority(1);  // normal default

    // Copy fence wait mode for debugging
    pSharedMem->SetFenceWaitMode(newConfig.fenceWaitMode);
    pSharedMem->SetUseGameQueue(newConfig.useGameQueue);

    // Copy FPS limiter settings
    pSharedMem->fpsLimiter.SetCaptureSyncEnabled(newConfig.fpsLimiter.captureSyncEnabled);
    pSharedMem->fpsLimiter.SetCaptureSyncMultiplier(newConfig.fpsLimiter.captureSyncMultiplier);
    pSharedMem->fpsLimiter.SetCaptureSyncLimiterMode(
        static_cast<uint32_t>(newConfig.fpsLimiter.captureSyncLimiterMode));
    pSharedMem->fpsLimiter.SetGeneralEnabled(newConfig.fpsLimiter.generalEnabled);
    pSharedMem->fpsLimiter.SetGeneralFps(newConfig.fpsLimiter.generalFps);
    pSharedMem->fpsLimiter.SetGeneralLimiterMode(static_cast<uint32_t>(newConfig.fpsLimiter.generalLimiterMode));
    pSharedMem->fpsLimiter.SetCaptureFps(newConfig.video.fps);
    pSharedMem->fpsLimiter.SetUseVFR(newConfig.video.useVFR);

    // Copy Graphics Config (SharedGraphicsConfig)
    {
        auto& dst = pSharedMem->graphicsConfig;
        const auto& src = newConfig.graphics;

        strncpy(dst.vsyncMode, src.vsyncMode.c_str(), sizeof(dst.vsyncMode) - 1);
        dst.vsyncMode[sizeof(dst.vsyncMode) - 1] = '\0';

        strncpy(dst.anisotropicFiltering, src.anisotropicFiltering.c_str(), sizeof(dst.anisotropicFiltering) - 1);
        dst.anisotropicFiltering[sizeof(dst.anisotropicFiltering) - 1] = '\0';

        strncpy(dst.samplerOverrideMode, src.samplerOverrideMode.c_str(), sizeof(dst.samplerOverrideMode) - 1);
        dst.samplerOverrideMode[sizeof(dst.samplerOverrideMode) - 1] = '\0';

        strncpy(dst.mipMapping, src.mipMapping.c_str(), sizeof(dst.mipMapping) - 1);
        dst.mipMapping[sizeof(dst.mipMapping) - 1] = '\0';

        strncpy(dst.mipBias, src.mipBias.c_str(), sizeof(dst.mipBias) - 1);
        dst.mipBias[sizeof(dst.mipBias) - 1] = '\0';

        strncpy(dst.mipBiasMode, src.mipBiasMode.c_str(), sizeof(dst.mipBiasMode) - 1);
        dst.mipBiasMode[sizeof(dst.mipBiasMode) - 1] = '\0';
        dst.forceMipBiasClamp = src.forceMipBiasClamp;

        strncpy(dst.msaaSamples, src.msaaSamples.c_str(), sizeof(dst.msaaSamples) - 1);
        dst.msaaSamples[sizeof(dst.msaaSamples) - 1] = '\0';
        dst.nvLodSpreadFix = src.nvLodSpreadFix;
        dst.forceRayReconstruction = src.forceRayReconstruction;
        dst.rayReconstructionOptimalSettings = src.rayReconstructionOptimalSettings;
        dst.disablePostProcessingEffects = src.disablePostProcessingEffects;
        dst.tonemapperSharpen = src.tonemapperSharpen;
        dst.internalFpsLimit = src.internalFpsLimit;
        dst.internalAnisotropicFiltering = src.internalAnisotropicFiltering;

        dst.prerenderLimit = src.cpuPrerenderLimit;
        dst.backbufferCount = src.backbufferCount;
        dst.sgssaa = src.sgssaa;
        dst.disableAutoMipBias = src.disableAutoMipBias;

        strncpy(dst.dlssAutoExposure, src.dlssAutoExposure.c_str(), sizeof(dst.dlssAutoExposure) - 1);
        dst.dlssAutoExposure[sizeof(dst.dlssAutoExposure) - 1] = '\0';

        strncpy(dst.dlssExposureNormalization, src.dlssExposureNormalization.c_str(),
                sizeof(dst.dlssExposureNormalization) - 1);
        dst.dlssExposureNormalization[sizeof(dst.dlssExposureNormalization) - 1] = '\0';

        // Synced parsed IDs for hook to use efficiently
        dst.dlssPresetDLAA = src.parsed.presetDLAA;
        dst.dlssPresetQuality = src.parsed.presetQuality;
        dst.dlssPresetBalanced = src.parsed.presetBalanced;
        dst.dlssPresetPerformance = src.parsed.presetPerformance;
        dst.dlssPresetUltraPerformance = src.parsed.presetUltraPerformance;
        dst.dlssPresetUltraQuality = src.parsed.presetUltraQuality;

        dst.dlssRRPresetDLAA = src.parsed.rrPresetDLAA;
        dst.dlssRRPresetQuality = src.parsed.rrPresetQuality;
        dst.dlssRRPresetBalanced = src.parsed.rrPresetBalanced;
        dst.dlssRRPresetPerformance = src.parsed.rrPresetPerformance;
        dst.dlssRRPresetUltraPerformance = src.parsed.rrPresetUltraPerformance;
        dst.dlssRRPresetUltraQuality = src.parsed.rrPresetUltraQuality;

        dst.dlssSRPreset = src.parsed.srPreset;
        dst.dlssRRPreset = src.parsed.rrPreset;

        if (src.parsed.srPreset > 0) {
            LogInfo("[IPC] Syncing SRPreset ID: %u", src.parsed.srPreset);
        }

        dst.dlssSharpening = src.parsed.dlssSharpening;
        dst.dlssFGFactor = src.parsed.dlssFGFactor;
    }

    pSharedMem->configVersion.fetch_add(1, std::memory_order_acq_rel);
}

bool IPCManager::GetLatestFrame(SharedMemoryLayout& outState) {
    if (!pSharedMem)
        return false;

    // Copy non-atomic fields manually to avoid copying atomics
    // The caller should use GetSharedMem() for direct atomic access
    outState.BeginWriteOverlayConfig();
    outState.overlayConfig = pSharedMem->ReadOverlayConfig();
    outState.EndWriteOverlayConfig();
    outState.SetHostPID(pSharedMem->GetHostPID());
    outState.SetRequestExit(pSharedMem->GetRequestExit());
    outState.SetDebugLogging(pSharedMem->GetDebugLogging());
    memcpy(outState.logFilePath, pSharedMem->logFilePath, sizeof(outState.logFilePath));
    outState.SetSharedHandle(0, pSharedMem->GetSharedHandle(0));
    outState.SetSharedHandle(1, pSharedMem->GetSharedHandle(1));
    outState.SetSharedHandle(2, pSharedMem->GetSharedHandle(2));
    outState.SetSharedHandle(3, pSharedMem->GetSharedHandle(3));
    outState.SetCurrentReadIndex(pSharedMem->GetCurrentReadIndex());
    outState.SetFenceShareHandle(pSharedMem->GetFenceShareHandle());
    outState.SetFenceValue(pSharedMem->GetFenceValue());
    outState.SetTimestamp(pSharedMem->GetTimestamp());
    outState.SetWidth(pSharedMem->GetWidth());
    outState.SetHeight(pSharedMem->GetHeight());
    outState.SetFormat(pSharedMem->GetFormat());
    outState.SetIsHDR(pSharedMem->GetIsHDR());
    outState.SetLuidLowPart(pSharedMem->GetLuidLowPart());
    outState.SetLuidHighPart(pSharedMem->GetLuidHighPart());
    outState.SetLuidSourcePid(pSharedMem->GetLuidSourcePid());
    outState.SetSourcePid(pSharedMem->GetSourcePid());
    // Copy CaptureState fields individually (can't copy struct with atomics)
    outState.runtimeState.captureRequested = pSharedMem->runtimeState.captureRequested.load();
    outState.runtimeState.isRecording = pSharedMem->runtimeState.isRecording.load();
    outState.runtimeState.recordingStartTime = pSharedMem->runtimeState.recordingStartTime.load();
    outState.runtimeState.currentFPS = pSharedMem->runtimeState.currentFPS.load();
    outState.runtimeState.gameFPS = pSharedMem->runtimeState.gameFPS.load();
    outState.runtimeState.hostDroppedFrames = pSharedMem->runtimeState.hostDroppedFrames.load();
    outState.runtimeState.duplicateFrames = pSharedMem->runtimeState.duplicateFrames.load();
    outState.runtimeState.lateFrames = pSharedMem->runtimeState.lateFrames.load();
    outState.runtimeState.encoderOverloadFlags = pSharedMem->runtimeState.encoderOverloadFlags.load();
    outState.runtimeState.encoderSustainFpsX100 = pSharedMem->runtimeState.encoderSustainFpsX100.load();
    outState.runtimeState.recordingHealthFlags = pSharedMem->runtimeState.recordingHealthFlags.load();
    outState.runtimeState.recordingTimelineDebtMs = pSharedMem->runtimeState.recordingTimelineDebtMs.load();
    outState.runtimeState.recordingPeakTimelineDebtMs = pSharedMem->runtimeState.recordingPeakTimelineDebtMs.load();
    outState.runtimeState.muxQueueBytes = pSharedMem->runtimeState.muxQueueBytes.load();
    outState.runtimeState.muxQueuePackets = pSharedMem->runtimeState.muxQueuePackets.load();
    outState.runtimeState.muxQueuePeakBytes = pSharedMem->runtimeState.muxQueuePeakBytes.load();
    outState.runtimeState.muxQueuePeakPackets = pSharedMem->runtimeState.muxQueuePeakPackets.load();
    outState.runtimeState.muxBackpressureCount = pSharedMem->runtimeState.muxBackpressureCount.load();
    outState.runtimeState.muxBackpressureWaitUs = pSharedMem->runtimeState.muxBackpressureWaitUs.load();
    outState.runtimeState.muxBackpressureMaxWaitUs = pSharedMem->runtimeState.muxBackpressureMaxWaitUs.load();
    outState.runtimeState.capturePhase = pSharedMem->runtimeState.capturePhase.load();
    outState.runtimeState.sourceFramesReceived = pSharedMem->runtimeState.sourceFramesReceived.load();
    outState.runtimeState.framesQueued = pSharedMem->runtimeState.framesQueued.load();
    outState.runtimeState.framesEncoded = pSharedMem->runtimeState.framesEncoded.load();
    outState.runtimeState.liveFramesEncoded = pSharedMem->runtimeState.liveFramesEncoded.load();
    outState.runtimeState.drainFramesEncoded = pSharedMem->runtimeState.drainFramesEncoded.load();
    outState.runtimeState.invalidFrameMetadata = pSharedMem->runtimeState.invalidFrameMetadata.load();
    outState.runtimeState.invalidSharedHandles = pSharedMem->runtimeState.invalidSharedHandles.load();
    outState.runtimeState.injectPacingDrops = pSharedMem->runtimeState.injectPacingDrops.load();
    outState.runtimeState.injectCadenceDrops = pSharedMem->runtimeState.injectCadenceDrops.load();
    outState.runtimeState.injectTrimmedFrames = pSharedMem->runtimeState.injectTrimmedFrames.load();
    outState.runtimeState.deferredFrames = pSharedMem->runtimeState.deferredFrames.load();
    outState.runtimeState.repeatedDeferredFrames = pSharedMem->runtimeState.repeatedDeferredFrames.load();
    outState.runtimeState.consecutiveDeferredFrames = pSharedMem->runtimeState.consecutiveDeferredFrames.load();
    outState.runtimeState.maxConsecutiveDeferredFrames = pSharedMem->runtimeState.maxConsecutiveDeferredFrames.load();
    outState.runtimeState.duplicateFramesNoSource = pSharedMem->runtimeState.duplicateFramesNoSource.load();
    outState.runtimeState.duplicateFramesDeferred = pSharedMem->runtimeState.duplicateFramesDeferred.load();
    outState.runtimeState.duplicateFramesTimerRebase = pSharedMem->runtimeState.duplicateFramesTimerRebase.load();
    outState.runtimeState.duplicateFramesDrain = pSharedMem->runtimeState.duplicateFramesDrain.load();
    outState.runtimeState.consecutiveDuplicateFrames = pSharedMem->runtimeState.consecutiveDuplicateFrames.load();
    outState.runtimeState.maxConsecutiveDuplicateFrames = pSharedMem->runtimeState.maxConsecutiveDuplicateFrames.load();
    outState.runtimeState.frameIndexRegressions = pSharedMem->runtimeState.frameIndexRegressions.load();
    outState.runtimeState.textureReuseAnomalies = pSharedMem->runtimeState.textureReuseAnomalies.load();
    outState.runtimeState.sourceTimestampRegressions = pSharedMem->runtimeState.sourceTimestampRegressions.load();
    outState.runtimeState.sourceTimestampStalls = pSharedMem->runtimeState.sourceTimestampStalls.load();
    outState.runtimeState.timerRebases = pSharedMem->runtimeState.timerRebases.load();
    outState.runtimeState.bufferedInjectDepthPeak = pSharedMem->runtimeState.bufferedInjectDepthPeak.load();
    outState.runtimeState.encoderQueuePeakDepth = pSharedMem->runtimeState.encoderQueuePeakDepth.load();
    outState.runtimeState.packetDurationClamps = pSharedMem->runtimeState.packetDurationClamps.load();
    outState.runtimeState.negativePtsCount = pSharedMem->runtimeState.negativePtsCount.load();
    outState.runtimeState.nonMonotonicPtsCount = pSharedMem->runtimeState.nonMonotonicPtsCount.load();
    outState.runtimeState.frameAgeAvgUs = pSharedMem->runtimeState.frameAgeAvgUs.load();
    outState.runtimeState.frameAgeMaxUs = pSharedMem->runtimeState.frameAgeMaxUs.load();
    outState.runtimeState.selectionErrorAvgUs = pSharedMem->runtimeState.selectionErrorAvgUs.load();
    outState.runtimeState.selectionErrorMaxUs = pSharedMem->runtimeState.selectionErrorMaxUs.load();
    outState.runtimeState.selectionErrorSignedAvgUs = pSharedMem->runtimeState.selectionErrorSignedAvgUs.load();
    outState.runtimeState.selectionEarlyMaxUs = pSharedMem->runtimeState.selectionEarlyMaxUs.load();
    outState.runtimeState.selectionLateMaxUs = pSharedMem->runtimeState.selectionLateMaxUs.load();
    outState.runtimeState.wgcSelectionErrorAvgUs = pSharedMem->runtimeState.wgcSelectionErrorAvgUs.load();
    outState.runtimeState.wgcSelectionErrorMaxUs = pSharedMem->runtimeState.wgcSelectionErrorMaxUs.load();
    outState.runtimeState.wgcSelectionErrorSignedAvgUs = pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.load();
    outState.runtimeState.wgcSelectionEarlyMaxUs = pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.load();
    outState.runtimeState.wgcSelectionLateMaxUs = pSharedMem->runtimeState.wgcSelectionLateMaxUs.load();
    outState.runtimeState.oldestBufferedFrameAgeUs = pSharedMem->runtimeState.oldestBufferedFrameAgeUs.load();
    outState.runtimeState.wgcSourceFrameIntervalAvgUs = pSharedMem->runtimeState.wgcSourceFrameIntervalAvgUs.load();
    outState.runtimeState.wgcSourceFrameJitterAvgUs = pSharedMem->runtimeState.wgcSourceFrameJitterAvgUs.load();
    outState.runtimeState.wgcSourceFrameJitterMaxUs = pSharedMem->runtimeState.wgcSourceFrameJitterMaxUs.load();
    outState.runtimeState.wgcSourceToCopyLatencyAvgUs = pSharedMem->runtimeState.wgcSourceToCopyLatencyAvgUs.load();
    outState.runtimeState.wgcSourceToCopyLatencyMaxUs = pSharedMem->runtimeState.wgcSourceToCopyLatencyMaxUs.load();
    outState.runtimeState.wgcTargetFps = pSharedMem->runtimeState.wgcTargetFps.load();
    outState.runtimeState.wgcDeliveredFramesPerSec = pSharedMem->runtimeState.wgcDeliveredFramesPerSec.load();
    outState.runtimeState.wgcDeliveredMin250Fps = pSharedMem->runtimeState.wgcDeliveredMin250Fps.load();
    outState.runtimeState.wgcDeliveredMin500Fps = pSharedMem->runtimeState.wgcDeliveredMin500Fps.load();
    outState.runtimeState.wgcInputMin250Fps = pSharedMem->runtimeState.wgcInputMin250Fps.load();
    outState.runtimeState.wgcInputMin500Fps = pSharedMem->runtimeState.wgcInputMin500Fps.load();
    outState.runtimeState.wgcAudioLeadExcessSamples = pSharedMem->runtimeState.wgcAudioLeadExcessSamples.load();
    outState.runtimeState.wgcQueueEmptyTickPermille = pSharedMem->runtimeState.wgcQueueEmptyTickPermille.load();
    outState.runtimeState.wgcBufferedAtTickAvgPermille = pSharedMem->runtimeState.wgcBufferedAtTickAvgPermille.load();
    outState.runtimeState.wgcBufferedAtTickMin = pSharedMem->runtimeState.wgcBufferedAtTickMin.load();
    outState.runtimeState.wgcStarvedTickCount = pSharedMem->runtimeState.wgcStarvedTickCount.load();
    outState.runtimeState.wgcSingleFrameTickCount = pSharedMem->runtimeState.wgcSingleFrameTickCount.load();
    outState.runtimeState.wgcCaptureHealthFlags = pSharedMem->runtimeState.wgcCaptureHealthFlags.load();
    outState.runtimeState.wgcCaptureHealthFps = pSharedMem->runtimeState.wgcCaptureHealthFps.load();
    // Atomic flags accessed directly, not copied
    // Note: atomics (throttleCapture, encoderQueueDepth, frameRing, logs) must be
    // accessed via GetSharedMem()
    return true;
}

void IPCManager::SendCaptureState(const CaptureState& state) {
    if (!pSharedMem)
        return;
    // Copy fields individually (can't copy struct with atomics)
    pSharedMem->runtimeState.captureRequested.store(state.captureRequested);
    pSharedMem->runtimeState.isRecording.store(state.isRecording);
    pSharedMem->runtimeState.recordingStartTime.store(state.recordingStartTime);
    pSharedMem->runtimeState.currentFPS.store(state.currentFPS);
    pSharedMem->runtimeState.gameFPS.store(state.gameFPS);
    pSharedMem->runtimeState.hostDroppedFrames.store(state.hostDroppedFrames);
}

void IPCManager::UpdateHostStats(uint32_t droppedFrames) {
    if (!pSharedMem)
        return;
    pSharedMem->runtimeState.hostDroppedFrames.store(droppedFrames);
}

void IPCManager::UpdateReadIndex(uint32_t readIndex) {
    if (!pSharedMem)
        return;
    localReadIndex = readIndex;
    pSharedMem->frameRing.readIndex.store(readIndex, std::memory_order_release);
}

uint32_t IPCManager::GetReadIndex() const {
    return localReadIndex;
}

SharedMemoryLayout* IPCManager::GetSharedMem() const {
    return pSharedMem;
}

ShmemBuffer* IPCManager::GetShmem() const {
    return pShmem;
}

void IPCManager::PollLogs() {
    if (!pSharedMem)
        return;

    // Simple ring buffer reader
    const uint32_t kMaxBacklog = 16;
    uint32_t writeIdx = pSharedMem->logs.writeIndex.load(std::memory_order_acquire);
    while (lastReadLogIndex < writeIdx) {
        // Don't read more than 16 lines back to avoid reading stale overwritten
        // data if we fell way behind (though in this design we just read
        // sequential) If we are way behind, jump to start?
        if (writeIdx - lastReadLogIndex > kMaxBacklog) {
            lastReadLogIndex = writeIdx - kMaxBacklog;
        }

        const char* msg = pSharedMem->logs.buffer[lastReadLogIndex % SharedMemoryLayout::LogBuffer::SLOT_COUNT];
        // Log it
        LogInfo("[Hook] %s", msg);

        lastReadLogIndex++;
    }
}

void IPCManager::UpdateThrottleState(uint32_t queueDepth, bool throttle) {
    if (!pSharedMem)
        return;
    pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
    pSharedMem->throttleCapture.store(throttle, std::memory_order_release);
}

void IPCManager::SignalExit() {
    if (!pSharedMem)
        return;
    pSharedMem->SetRequestExit(true);
}
