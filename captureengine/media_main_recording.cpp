#include "media_main_internal.h"

#include "../common/live_stream_config.h"

void StampAvSyncStatus(AppConfig& config, const char* confidence, const char* reason, float resolvedMs,
                              bool usedAudioProbe) {
    config.avSyncConfidence = confidence ? confidence : "low";
    config.avSyncReason = reason ? reason : "unknown";
    config.avSyncResolvedRenderLatencyMs = resolvedMs;
    config.avSyncUsedAudioProbe = usedAudioProbe;
}

// Apply the auto-detected render-endpoint latency to render-domain sources (system loopback + app
// process loopback only). No-op when autodetect is off, a manual override is configured, or no
// value has been measured. Microphones (Domain 2) are never touched here. Cheap and idempotent.
void ApplyAutoDetectedRenderLatencyToConfig(AppConfig& config) {
    int renderDomainSources = 0;
    int micDomainSources = 0;
    for (const auto& s : config.audioSources) {
        const bool renderDomain = s.sourceType == AudioConfig::SystemAudio || s.sourceType == AudioConfig::AppAudio;
        if (renderDomain) {
            ++renderDomainSources;
        } else if (s.sourceType == AudioConfig::Microphone) {
            ++micDomainSources;
        }
    }

    if (config.audioCaptureLatencyMs > 0.0f) {
        StampAvSyncStatus(config, "medium", "manual_config_override", config.audioCaptureLatencyMs, false);
        LogInfo(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=%.3f "
            "autodetect=%d probe=skipped chosenDelayMs=%.3f confidence=medium reason=manual_config_override",
            renderDomainSources, micDomainSources, static_cast<double>(config.audioCaptureLatencyMs),
            config.audioLatencyAutodetect ? 1 : 0, static_cast<double>(config.audioCaptureLatencyMs));
        return;
    }

    if (!config.audioLatencyAutodetect) {
        StampAvSyncStatus(config, "low", "autodetect_disabled", 0.0f, false);
        LogWarn(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 "
            "autodetect=0 probe=disabled chosenDelayMs=0.000 confidence=low reason=autodetect_disabled",
            renderDomainSources, micDomainSources);
        return;
    }

    if (media_main_g_AutoDetectedRenderLatencyMs <= 0.0) {
        StampAvSyncStatus(config, media_main_g_AvSyncConfidence.c_str(), media_main_g_AvSyncReason.c_str(), 0.0f, media_main_g_AvSyncUsedAudioProbe);
        LogWarn(
            "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 "
            "autodetect=1 probe=%s chosenDelayMs=0.000 confidence=%s reason=%s",
            renderDomainSources, micDomainSources, media_main_g_RenderLatencyMeasureAttempted ? "unavailable" : "not_attempted",
            config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
        return;
    }
    const float ms = static_cast<float>(media_main_g_AutoDetectedRenderLatencyMs);
    config.audioCaptureLatencyMs = ms;
    int applied = 0;
    for (auto& s : config.audioSources) {
        const bool renderDomain = s.sourceType == AudioConfig::SystemAudio || s.sourceType == AudioConfig::AppAudio;
        if (renderDomain && s.captureLatencyMs == 0.0f) {  // inherited auto default, no per-source override
            s.captureLatencyMs = ms;
            ++applied;
        }
    }
    StampAvSyncStatus(config, media_main_g_AvSyncConfidence.c_str(), media_main_g_AvSyncReason.c_str(), ms, media_main_g_AvSyncUsedAudioProbe);
    LogInfo(
        "[AVSyncAuto] passiveInputs renderSources=%d micSources=%d generalRenderLatencyMs=0.000 autodetect=1 "
        "probe=%s chosenDelayMs=%.3f appliedSources=%d confidence=%s reason=%s domain=render_endpoint",
        renderDomainSources, micDomainSources,
        config.avSyncUsedAudioProbe ? "audio_render_loopback" : "memory_cache_or_manual", static_cast<double>(ms),
        applied, config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
}

void DeleteLegacyAudioLatencyCacheFileOnce(const std::string& cacheDir) {
    if (cacheDir.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(media_main_g_LegacyAudioLatencyCacheCleanupMutex);
        if (media_main_g_LegacyAudioLatencyCacheCleanupAttempted) {
            return;
        }
        media_main_g_LegacyAudioLatencyCacheCleanupAttempted = true;
    }

    std::string path = cacheDir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '\\';
    }
    path += "audio_latency_cache.ini";

    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    if (DeleteFileA(path.c_str())) {
        LogInfo("[AVSyncProbe] legacyDiskCache=deleted cacheMode=memory file=audio_latency_cache.ini");
    } else {
        LogInfo("[AVSyncProbe] legacyDiskCache=delete_failed cacheMode=memory file=audio_latency_cache.ini error=%lu",
                static_cast<unsigned long>(GetLastError()));
    }
}

// Perform the one-time product-safe audio-only render->loopback measurement. Call only when NOT
// recording. On a cache miss it may render a near-inaudible marker; it never opens a calibration
// window or emits a video stimulus. Safe to call repeatedly; it runs at most once per process and
// is a cheap memory cache hit otherwise.
void MeasureRenderLatencyOnce(const AppConfig& config, const std::string& cacheDir) {
    DeleteLegacyAudioLatencyCacheFileOnce(cacheDir);
    if (media_main_g_RenderLatencyMeasureAttempted) {
        return;
    }
    if (config.audioCaptureLatencyMs > 0.0f) {
        media_main_g_RenderLatencyMeasureAttempted = true;  // disabled or manual override: never measure
        media_main_g_AutoDetectedRenderLatencyMs = -1.0;
        media_main_g_AvSyncConfidence = "medium";
        media_main_g_AvSyncReason = "manual_config_override";
        media_main_g_AvSyncUsedAudioProbe = false;
        LogInfo("[AVSyncAuto] probe=skipped confidence=medium reason=manual_config_override configuredDelayMs=%.3f",
                static_cast<double>(config.audioCaptureLatencyMs));
        return;
    }
    if (!config.audioLatencyAutodetect) {
        media_main_g_RenderLatencyMeasureAttempted = true;
        media_main_g_AutoDetectedRenderLatencyMs = -1.0;
        media_main_g_AvSyncConfidence = "low";
        media_main_g_AvSyncReason = "autodetect_disabled";
        media_main_g_AvSyncUsedAudioProbe = false;
        LogWarn("[AVSyncAuto] probe=disabled confidence=low reason=autodetect_disabled chosenDelayMs=0.000");
        return;
    }
    media_main_g_RenderLatencyMeasureAttempted = true;

    // Audio-only render->loopback probe (Start-anchor) - the default active auto-detect.
    double ms = 0.0;
    if (MediaEngine_MeasureRenderEndpointLatency &&
        MediaEngine_MeasureRenderEndpointLatency(cacheDir.c_str(), false, &ms) && ms > 0.0) {
        media_main_g_AutoDetectedRenderLatencyMs = ms;
        media_main_g_AvSyncConfidence = "high";
        media_main_g_AvSyncReason = "audio_probe_render_loopback";
        media_main_g_AvSyncUsedAudioProbe = true;
        LogInfo("[AVSyncAuto] probe=audio_render_loopback chosenDelayMs=%.3f confidence=high domain=render_endpoint",
                ms);
    } else {
        media_main_g_AutoDetectedRenderLatencyMs = -1.0;
        media_main_g_AvSyncConfidence = "low";
        media_main_g_AvSyncReason = "probe_unavailable_passive_insufficient";
        media_main_g_AvSyncUsedAudioProbe = false;
        LogWarn(
            "[AVSyncAuto] probe=unavailable chosenDelayMs=0.000 confidence=low "
            "reason=probe_unavailable_passive_insufficient");
    }
}

bool MediaAudioConfigEquals(const AudioConfig& lhs, const AudioConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.device == rhs.device && lhs.processName == rhs.processName &&
           lhs.processId == rhs.processId && lhs.sourceType == rhs.sourceType && lhs.tracks == rhs.tracks &&
           lhs.codec == rhs.codec && lhs.bitrate == rhs.bitrate && lhs.sampleRate == rhs.sampleRate &&
           lhs.bitDepth == rhs.bitDepth && lhs.downmix == rhs.downmix && lhs.captureLatencyMs == rhs.captureLatencyMs;
}

bool MediaScalingConfigEquals(const ScalingConfig& lhs, const ScalingConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.outputResolution == rhs.outputResolution && lhs.quality == rhs.quality &&
           lhs.sharpness == rhs.sharpness && lhs.outputWidth == rhs.outputWidth && lhs.outputHeight == rhs.outputHeight;
}

bool MediaVideoConfigEquals(const VideoConfig& lhs, const VideoConfig& rhs) {
    return lhs.encoder == rhs.encoder && lhs.fps == rhs.fps && lhs.container == rhs.container &&
           lhs.outputDir == rhs.outputDir && lhs.rateControl == rhs.rateControl && lhs.bitrate == rhs.bitrate &&
           lhs.maxBitrate == rhs.maxBitrate && lhs.bufferSize == rhs.bufferSize &&
           lhs.keyframeInterval == rhs.keyframeInterval &&
           lhs.preset == rhs.preset && lhs.tuning == rhs.tuning && lhs.multipass == rhs.multipass &&
           lhs.splitEncode == rhs.splitEncode && lhs.profile == rhs.profile && lhs.lookahead == rhs.lookahead &&
           lhs.spatialAq == rhs.spatialAq && lhs.temporalAq == rhs.temporalAq &&
           lhs.aqStrength == rhs.aqStrength && lhs.bFrames == rhs.bFrames && lhs.bRefMode == rhs.bRefMode &&
           lhs.customOptions == rhs.customOptions && lhs.captureCursor == rhs.captureCursor &&
           lhs.faceCamera == rhs.faceCamera && lhs.qp == rhs.qp && lhs.amfUsage == rhs.amfUsage &&
           lhs.amfPreset == rhs.amfPreset && lhs.amfQp == rhs.amfQp && lhs.amfAsyncDepth == rhs.amfAsyncDepth &&
           lhs.amfPreencode == rhs.amfPreencode && lhs.amfPreanalysis == rhs.amfPreanalysis &&
           lhs.amfLookahead == rhs.amfLookahead && lhs.amfSpatialAq == rhs.amfSpatialAq &&
           lhs.amfTemporalAq == rhs.amfTemporalAq && lhs.amfAqStrength == rhs.amfAqStrength &&
           lhs.amfHighMotionQualityBoost == rhs.amfHighMotionQualityBoost &&
           lhs.amfBRefMode == rhs.amfBRefMode && lhs.amfEnforceHrd == rhs.amfEnforceHrd &&
           lhs.amfFillerData == rhs.amfFillerData && lhs.qsvPreset == rhs.qsvPreset && lhs.qsvQp == rhs.qsvQp &&
           lhs.qsvAsyncDepth == rhs.qsvAsyncDepth && lhs.qsvLowPower == rhs.qsvLowPower &&
           lhs.qsvLookahead == rhs.qsvLookahead && lhs.qsvMbbRc == rhs.qsvMbbRc && lhs.qsvExtBrc == rhs.qsvExtBrc &&
           lhs.qsvAdaptiveI == rhs.qsvAdaptiveI && lhs.qsvAdaptiveB == rhs.qsvAdaptiveB &&
           lhs.qsvLowDelayBrc == rhs.qsvLowDelayBrc && lhs.qsvScenario == rhs.qsvScenario &&
           lhs.mfRateControl == rhs.mfRateControl && lhs.mfQuality == rhs.mfQuality &&
           lhs.mfScenario == rhs.mfScenario && lhs.mfHwEncoding == rhs.mfHwEncoding &&
           lhs.mfQualityVsSpeed == rhs.mfQualityVsSpeed && lhs.mfLowLatency == rhs.mfLowLatency &&
           lhs.gpuPriority == rhs.gpuPriority && lhs.bitDepth == rhs.bitDepth && lhs.colorSpace == rhs.colorSpace &&
           lhs.colorRange == rhs.colorRange && lhs.chromaSubsampling == rhs.chromaSubsampling &&
           lhs.hdrNominalPeakNits == rhs.hdrNominalPeakNits &&
           lhs.useVFR == rhs.useVFR && lhs.useVFR_AudioSync == rhs.useVFR_AudioSync &&
           MediaScalingConfigEquals(lhs.scaling, rhs.scaling);

}

bool MediaEngineConfigEquals(const AppConfig& lhs, const AppConfig& rhs) {
    if (lhs.logLevel != rhs.logLevel || lhs.captureMethod != rhs.captureMethod ||
        lhs.captureMonitor != rhs.captureMonitor ||
        lhs.autoFullscreenPrefersDxgiDup != rhs.autoFullscreenPrefersDxgiDup ||
        lhs.wgcSkipSplitDeviceFlush != rhs.wgcSkipSplitDeviceFlush ||
        lhs.wgcSameDeviceCapture != rhs.wgcSameDeviceCapture ||
        lhs.wgcSmoothnessBufferEnabled != rhs.wgcSmoothnessBufferEnabled ||
        lhs.wgcSmoothnessBufferMaxMs != rhs.wgcSmoothnessBufferMaxMs ||
        lhs.wgcSmoothnessBufferVramBudgetMb != rhs.wgcSmoothnessBufferVramBudgetMb ||
        lhs.wgcVideoMemoryReservation != rhs.wgcVideoMemoryReservation ||
        lhs.wgcAllowLossyBgra8Pool != rhs.wgcAllowLossyBgra8Pool || !MediaVideoConfigEquals(lhs.video, rhs.video) ||
        lhs.audioSources.size() != rhs.audioSources.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.audioSources.size(); ++i) {
        if (!MediaAudioConfigEquals(lhs.audioSources[i], rhs.audioSources[i])) {
            return false;
        }
    }

    return true;
}

bool IsExplicitTenBitVideo(const VideoConfig& video) {
    return _stricmp(video.bitDepth.c_str(), "10") == 0;
}

uint32_t GetInitialWgcCfrTargetFps(const VideoConfig& video) {
    if (video.useVFR || video.fps <= 0) {
        return 0;
    }

    return ce::capture_policy::GetWgcCfrProducerTargetFps(static_cast<uint32_t>(video.fps));
}

uint32_t SaturatingToUint32(uint64_t value) {
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

void SetCapturePipelinePhase(CapturePipelinePhase phase) {
    if (!media_main_g_pSharedMem) {
        return;
    }
    media_main_g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(phase), std::memory_order_release);
}

bool TryArmCapturePipelineWarmup() {
    return !media_main_g_pSharedMem ||
           ce::recording_lifecycle::TryArmWarmup(media_main_g_pSharedMem->runtimeState.capturePhase, media_main_g_Recording);
}

bool TryCommitCapturePipelineLive() {
    return !media_main_g_pSharedMem || ce::recording_lifecycle::TryCommitLive(media_main_g_pSharedMem->runtimeState.capturePhase, media_main_g_Recording);
}

CapturePipelinePhase BeginCapturePipelineStop() {
    if (!media_main_g_pSharedMem) {
        return CapturePipelinePhase::kCancelling;
    }
    const uint32_t liveFrames = media_main_g_pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_acquire);
    return ce::recording_lifecycle::BeginStop(media_main_g_pSharedMem->runtimeState.capturePhase, liveFrames);
}

void ResetRuntimeDiagnostics(SharedMemoryLayout* sharedMem) {
    if (!sharedMem) {
        return;
    }

    auto& state = sharedMem->runtimeState;
    state.currentFPS.store(0.0, std::memory_order_relaxed);
    state.gameFPS.store(0.0, std::memory_order_relaxed);
    state.hostDroppedFrames.store(0, std::memory_order_relaxed);
    state.duplicateFrames.store(0, std::memory_order_relaxed);
    state.lateFrames.store(0, std::memory_order_relaxed);
    state.encoderOverloadFlags.store(0, std::memory_order_relaxed);
    state.encoderSustainFpsX100.store(0, std::memory_order_relaxed);
    state.muxQueueBytes.store(0, std::memory_order_relaxed);
    state.muxQueuePackets.store(0, std::memory_order_relaxed);
    state.muxQueuePeakBytes.store(0, std::memory_order_relaxed);
    state.muxQueuePeakPackets.store(0, std::memory_order_relaxed);
    state.muxBackpressureCount.store(0, std::memory_order_relaxed);
    state.muxBackpressureWaitUs.store(0, std::memory_order_relaxed);
    state.muxBackpressureMaxWaitUs.store(0, std::memory_order_relaxed);
    state.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kIdle), std::memory_order_release);
    state.sourceFramesReceived.store(0, std::memory_order_relaxed);
    state.framesQueued.store(0, std::memory_order_relaxed);
    state.framesEncoded.store(0, std::memory_order_relaxed);
    state.liveFramesEncoded.store(0, std::memory_order_relaxed);
    state.drainFramesEncoded.store(0, std::memory_order_relaxed);
    state.invalidFrameMetadata.store(0, std::memory_order_relaxed);
    state.invalidSharedHandles.store(0, std::memory_order_relaxed);
    state.injectPacingDrops.store(0, std::memory_order_relaxed);
    state.injectCadenceDrops.store(0, std::memory_order_relaxed);
    state.injectTrimmedFrames.store(0, std::memory_order_relaxed);
    state.injectProducerCaptureLockDrops.store(0, std::memory_order_relaxed);
    state.injectProducerCpuLeaseBusyDrops.store(0, std::memory_order_relaxed);
    state.injectProducerGpuBusyDrops.store(0, std::memory_order_relaxed);
    state.injectProducerMetadataFullDrops.store(0, std::memory_order_relaxed);
    state.injectFrameReadySignals.store(0, std::memory_order_relaxed);
    state.injectPublicationToIngestAvgUs.store(0, std::memory_order_relaxed);
    state.injectPublicationToIngestMaxUs.store(0, std::memory_order_relaxed);
    state.encoderTimerWakeLateAvgUs.store(0, std::memory_order_relaxed);
    state.encoderTimerWakeLateMaxUs.store(0, std::memory_order_relaxed);
    state.deferredFrames.store(0, std::memory_order_relaxed);
    state.repeatedDeferredFrames.store(0, std::memory_order_relaxed);
    state.consecutiveDeferredFrames.store(0, std::memory_order_relaxed);
    state.maxConsecutiveDeferredFrames.store(0, std::memory_order_relaxed);
    state.duplicateFramesNoSource.store(0, std::memory_order_relaxed);
    state.duplicateFramesDeferred.store(0, std::memory_order_relaxed);
    state.duplicateFramesTimerRebase.store(0, std::memory_order_relaxed);
    state.duplicateFramesDrain.store(0, std::memory_order_relaxed);
    state.consecutiveDuplicateFrames.store(0, std::memory_order_relaxed);
    state.maxConsecutiveDuplicateFrames.store(0, std::memory_order_relaxed);
    state.frameIndexRegressions.store(0, std::memory_order_relaxed);
    state.textureReuseAnomalies.store(0, std::memory_order_relaxed);
    state.sourceTimestampRegressions.store(0, std::memory_order_relaxed);
    state.sourceTimestampStalls.store(0, std::memory_order_relaxed);
    state.timerRebases.store(0, std::memory_order_relaxed);
    state.bufferedInjectDepthPeak.store(0, std::memory_order_relaxed);
    state.encoderQueuePeakDepth.store(0, std::memory_order_relaxed);
    state.packetDurationClamps.store(0, std::memory_order_relaxed);
    state.negativePtsCount.store(0, std::memory_order_relaxed);
    state.nonMonotonicPtsCount.store(0, std::memory_order_relaxed);
    state.frameAgeAvgUs.store(0, std::memory_order_relaxed);
    state.frameAgeMaxUs.store(0, std::memory_order_relaxed);
    state.selectionErrorAvgUs.store(0, std::memory_order_relaxed);
    state.selectionErrorMaxUs.store(0, std::memory_order_relaxed);
    state.selectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
    state.selectionEarlyMaxUs.store(0, std::memory_order_relaxed);
    state.selectionLateMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    state.oldestBufferedFrameAgeUs.store(0, std::memory_order_relaxed);
    state.wgcSourceFrameIntervalAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSourceFrameJitterAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSourceFrameJitterMaxUs.store(0, std::memory_order_relaxed);
    state.wgcSourceToCopyLatencyAvgUs.store(0, std::memory_order_relaxed);
    state.wgcSourceToCopyLatencyMaxUs.store(0, std::memory_order_relaxed);
    state.wgcTargetFps.store(0, std::memory_order_relaxed);
    state.wgcDeliveredFramesPerSec.store(0, std::memory_order_relaxed);
    state.wgcDeliveredMin250Fps.store(0, std::memory_order_relaxed);
    state.wgcDeliveredMin500Fps.store(0, std::memory_order_relaxed);
    state.wgcInputMin250Fps.store(0, std::memory_order_relaxed);
    state.wgcInputMin500Fps.store(0, std::memory_order_relaxed);
    state.wgcQueueEmptyTickPermille.store(0, std::memory_order_relaxed);
    state.wgcBufferedAtTickAvgPermille.store(0, std::memory_order_relaxed);
    state.wgcBufferedAtTickMin.store(0, std::memory_order_relaxed);
    state.wgcStarvedTickCount.store(0, std::memory_order_relaxed);
    state.wgcSingleFrameTickCount.store(0, std::memory_order_relaxed);
    state.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
    state.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
    state.encoderBottlenecked.store(0, std::memory_order_relaxed);
    state.recordingTimelineDebtMs.store(0, std::memory_order_relaxed);
    state.recordingPeakTimelineDebtMs.store(0, std::memory_order_relaxed);
    state.recordingHealthFlags.store(0, std::memory_order_release);
}

void ResetRecordingHealthPublication() {
    media_main_g_RecordingTimelineDebtMs.store(0, std::memory_order_relaxed);
    media_main_g_RecordingPeakTimelineDebtMs.store(0, std::memory_order_relaxed);
    media_main_g_RecordingCapacityAttributedDebtMs.store(0, std::memory_order_relaxed);
    media_main_g_RecordingHealthFlags.store(0, std::memory_order_release);
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.recordingTimelineDebtMs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingPeakTimelineDebtMs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingHealthFlags.store(0, std::memory_order_release);
    }
}

void PublishRecordingHealth(const ce::capture_policy::RecordingHealthState& health) {
    media_main_g_RecordingTimelineDebtMs.store(health.currentDebtMs, std::memory_order_relaxed);
    media_main_g_RecordingPeakTimelineDebtMs.store(health.peakDebtMs, std::memory_order_relaxed);
    media_main_g_RecordingCapacityAttributedDebtMs.store(health.capacityAttributedDebtMs, std::memory_order_relaxed);
    media_main_g_RecordingHealthFlags.store(health.flags, std::memory_order_release);
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.recordingTimelineDebtMs.store(health.currentDebtMs, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingPeakTimelineDebtMs.store(health.peakDebtMs, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.recordingHealthFlags.store(health.flags, std::memory_order_release);
    }
}

void CompleteRecordingFinalization(bool canceled, bool outputSaved) {
    const bool liveStream = media_main_g_LiveStreamRecording.exchange(false, std::memory_order_acq_rel);
    const uint32_t healthFlags = media_main_g_RecordingHealthFlags.load(std::memory_order_acquire);
    const uint32_t currentDebtMs = media_main_g_RecordingTimelineDebtMs.load(std::memory_order_relaxed);
    const uint32_t peakDebtMs = media_main_g_RecordingPeakTimelineDebtMs.load(std::memory_order_relaxed);
    const uint32_t capacityAttributedDebtMs =
        media_main_g_RecordingCapacityAttributedDebtMs.load(std::memory_order_relaxed);
    const char* healthStatus = ce::capture_policy::GetRecordingHealthStatus(healthFlags);
    const char* healthCause = ce::capture_policy::GetRecordingHealthCause(healthFlags);
    LogInfo(
        "[RECORDING FINALIZATION] mode=%s status=%s health=%s cause=%s flags=0x%X currentDebtMs=%u peakDebtMs=%u "
        "capacityDebtMs=%u outputSaved=%d finalizationComplete=1 settingsChanged=0",
        liveStream ? "stream" : "recording",
        canceled ? "canceled" : (outputSaved ? "media_finalized" : "failed"), healthStatus, healthCause,
        healthFlags, currentDebtMs, peakDebtMs, capacityAttributedDebtMs, outputSaved ? 1 : 0);
    FinalizeRecordingManifest(media_main_g_RecordingManifestLogPath, canceled, outputSaved, healthStatus, healthCause,
                              healthFlags, currentDebtMs, peakDebtMs, capacityAttributedDebtMs);

    if (!media_main_g_pSharedMem) {
        return;
    }
    auto& state = media_main_g_pSharedMem->runtimeState;
    const bool newerRecordingActive = state.captureRequested.load(std::memory_order_acquire) ||
                                      state.isRecording.load(std::memory_order_acquire) ||
                                      state.GetRecordingStartIntent() != RecordingStartIntent::Idle;
    if (newerRecordingActive) {
        LogInfo("[RECORDING FINALIZATION] Completion notification suppressed because a newer recording is active");
        return;
    }

    const bool degraded = ce::capture_policy::HasRecordingHealthFlag(
        healthFlags, ce::capture_policy::kRecordingHealthFlagVideoDegraded);
    const OverlayNotificationType notification =
        ce::live_stream::SelectOutputCompletionNotification(liveStream, canceled, outputSaved, degraded);
    state.notificationType.store(static_cast<uint32_t>(notification), std::memory_order_release);
    state.notificationExpiry.store(GetTickCount64() + ((degraded || !outputSaved) ? 7000ULL : 3000ULL),
                                   std::memory_order_release);
}

bool IsActiveScreenGrab() {
    return media_main_g_UseScreenGrab.load(std::memory_order_acquire);
}

void SetActiveScreenGrab(bool enabled) {
    media_main_g_UseScreenGrab.store(enabled, std::memory_order_release);
    if (MediaEngine_SetActiveScreenGrab) {
        MediaEngine_SetActiveScreenGrab(enabled);
    }
}

bool IsPreferredScreenGrab() {
    return media_main_g_PreferScreenGrab.load(std::memory_order_acquire);
}

void SetPreferredScreenGrab(bool enabled) {
    media_main_g_PreferScreenGrab.store(enabled, std::memory_order_release);
}

void SetCaptureRequestedState(bool enabled) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    media_main_g_pSharedMem->runtimeState.captureRequested.store(enabled, std::memory_order_release);
}

void SetInjectVideoCaptureRequestedState(bool enabled, const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    const bool previous = media_main_g_pSharedMem->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested);
    media_main_g_pSharedMem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, enabled);
    if (previous != enabled) {
        LogInfo("[Media] Inject video publication %s (%s)", enabled ? "enabled" : "disabled",
                reason ? reason : "unspecified");
    }
}

// The media half of the recording-status overlay protocol lives in
// captureengine/status_overlay_sync.h; these thin wrappers keep the shared-memory
// null-check with the rest of the publication helpers.
void SignalStatusOverlaySync() {
    ce::status_overlay::SignalSync();
}

void RequestStatusOverlayDarkForCapture(const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }
    ce::status_overlay::RequestDarkForCapture(media_main_g_pSharedMem->runtimeState, reason);
}

void ReleaseStatusOverlayDarkForCapture(const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }
    ce::status_overlay::ReleaseDarkForCapture(media_main_g_pSharedMem->runtimeState, reason);
}

void SetRecordingVisibleState(bool enabled) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    if (enabled) {
        const bool wasVisible = media_main_g_pSharedMem->runtimeState.isRecording.exchange(true, std::memory_order_acq_rel);
        if (!wasVisible) {
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            media_main_g_pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64(), std::memory_order_release);
        }
        // Propagate audio-only flag so overlay can show AUDIO vs REC
        media_main_g_pSharedMem->runtimeState.audioOnly.store(media_main_g_AudioOnly, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
        ReleaseStatusOverlayDarkForCapture("recording live");
    } else {
        media_main_g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.audioOnly.store(false, std::memory_order_release);
        ReleaseStatusOverlayDarkForCapture("recording not live");
    }
    // Publish the resolved status before waking the overlay so it renders the final state.
    SignalStatusOverlaySync();
}

void PublishRecordingStartFailure(RecordingFailureCode failureCode, const char* reason) {
    const bool liveStream = media_main_g_LiveStreamRecording.load(std::memory_order_acquire);
    if (media_main_g_pSharedMem) {
        ReleaseStatusOverlayDarkForCapture("recording start failure");
        media_main_g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
        media_main_g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        StoreRelease(media_main_g_pSharedMem->runtimeState.recordingFailureCode, static_cast<uint32_t>(failureCode));
        // Surface the failed start in the inject and pseudo overlays through the
        // same transient notification channel finalization uses. Both overlays
        // show the notification only in the idle state, which the intent and
        // isRecording resets above have already established.
        media_main_g_pSharedMem->runtimeState.notificationType.store(
            static_cast<uint32_t>(liveStream ? OverlayNotificationType::StreamingFailed
                                             : OverlayNotificationType::RecordingFailed),
            std::memory_order_release);
        media_main_g_pSharedMem->runtimeState.notificationExpiry.store(GetTickCount64() + 7000ULL, std::memory_order_release);
        SignalStatusOverlaySync();
    }
    LogError("[Media] %s start failed: %s (code=%u)", liveStream ? "Stream" : "Recording",
             reason ? reason : "unspecified",
             static_cast<uint32_t>(failureCode));
}

bool JoinThreadWithTimeout(std::thread& thread, DWORD timeoutMs, const char* threadName) {
    if (!thread.joinable()) {
        return true;
    }

    HANDLE threadHandle = ce::Win32ThreadHandle(thread);
    DWORD waitResult = WaitForSingleObject(threadHandle, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        thread.join();
        return true;
    }

    if (waitResult == WAIT_TIMEOUT) {
        LogWarn(
            "[Media] Timeout waiting for %s thread (%lu ms); preserving ownership and continuing to wait because "
            "cleanup while the worker is live would race released capture/encoder resources",
            threadName, static_cast<unsigned long>(timeoutMs));
    } else {
        LogWarn(
            "[Media] WaitForSingleObject failed for %s thread (error=%lu); preserving ownership and joining "
            "synchronously",
            threadName, GetLastError());
    }

    thread.join();
    LogInfo("[Media] %s thread eventually joined after the bounded wait", threadName);
    return true;
}

void MediaLogCallback(const char* msg) {
    LogInfo("[Media] %s", msg);
}
