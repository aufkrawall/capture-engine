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

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void SetCapturePipelinePhase(CapturePipelinePhase phase) {
    if (!g_pSharedMem) {
        return;
    }
    g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(phase), std::memory_order_release);
}

bool TryArmCapturePipelineWarmup() {
    return !g_pSharedMem ||
           ce::recording_lifecycle::TryArmWarmup(g_pSharedMem->runtimeState.capturePhase, g_Recording);
}

bool TryCommitCapturePipelineLive() {
    return !g_pSharedMem || ce::recording_lifecycle::TryCommitLive(g_pSharedMem->runtimeState.capturePhase, g_Recording);
}

CapturePipelinePhase BeginCapturePipelineStop() {
    if (!g_pSharedMem) {
        return CapturePipelinePhase::kCancelling;
    }
    const uint32_t liveFrames = g_pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_acquire);
    return ce::recording_lifecycle::BeginStop(g_pSharedMem->runtimeState.capturePhase, liveFrames);
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
}

bool IsActiveScreenGrab() {
    return g_UseScreenGrab.load(std::memory_order_acquire);
}

void SetActiveScreenGrab(bool enabled) {
    g_UseScreenGrab.store(enabled, std::memory_order_release);
    if (MediaEngine_SetActiveScreenGrab) {
        MediaEngine_SetActiveScreenGrab(enabled);
    }
}

bool IsPreferredScreenGrab() {
    return g_PreferScreenGrab.load(std::memory_order_acquire);
}

void SetPreferredScreenGrab(bool enabled) {
    g_PreferScreenGrab.store(enabled, std::memory_order_release);
}

void SetCaptureRequestedState(bool enabled) {
    if (!g_pSharedMem) {
        return;
    }

    g_pSharedMem->runtimeState.captureRequested.store(enabled, std::memory_order_release);
}

void SetInjectVideoCaptureRequestedState(bool enabled, const char* reason) {
    if (!g_pSharedMem) {
        return;
    }

    const bool previous = g_pSharedMem->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested);
    g_pSharedMem->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested, enabled);
    if (previous != enabled) {
        LogInfo("[Media] Inject video publication %s (%s)", enabled ? "enabled" : "disabled",
                reason ? reason : "unspecified");
    }
}

void SetRecordingVisibleState(bool enabled) {
    if (!g_pSharedMem) {
        return;
    }

    if (enabled) {
        const bool wasVisible = g_pSharedMem->runtimeState.isRecording.exchange(true, std::memory_order_acq_rel);
        if (!wasVisible) {
            g_pSharedMem->runtimeState.recordingStartTime.store(GetTickCount64(), std::memory_order_release);
        }
        // Propagate audio-only flag so overlay can show AUDIO vs REC
        g_pSharedMem->runtimeState.audioOnly.store(g_AudioOnly, std::memory_order_release);
        g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
    } else {
        g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        g_pSharedMem->runtimeState.audioOnly.store(false, std::memory_order_release);
    }
}

void PublishRecordingStartFailure(RecordingFailureCode failureCode, const char* reason) {
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
        g_pSharedMem->runtimeState.isRecording.store(false, std::memory_order_release);
        g_pSharedMem->runtimeState.recordingStartTime.store(0, std::memory_order_release);
        StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode, static_cast<uint32_t>(failureCode));
    }
    LogError("[Media] Recording start failed: %s (code=%u)", reason ? reason : "unspecified",
             static_cast<uint32_t>(failureCode));
}

bool WindowBelongsToProcess(HWND hwnd, DWORD pid) {
    if (!hwnd || pid == 0) {
        return false;
    }

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    return windowPid == pid;
}

bool ShouldPreferInjectCaptureForFullscreenWindow(HWND hwnd, DWORD pid) {
    return WindowBelongsToProcess(hwnd, pid) && IsWindowFullscreenLike(hwnd);
}

InjectFrameLineage MakeInjectFrameLineage(const QueuedFrame& frame) {
    InjectFrameLineage lineage;
    lineage.frameIndex = frame.frameIndex;
    lineage.textureIndex = frame.textureIndex;
    lineage.fenceValue = frame.fenceValue;
    lineage.ringIndex = frame.ringIndex;
    lineage.timestamp = frame.timestamp;
    lineage.enqueueQpc = frame.enqueueQpc;
    lineage.deferCount = frame.deferCount;
    return lineage;
}

bool MatchesInjectFrameLineage(const QueuedFrame& frame, const InjectFrameLineage& lineage) {
    return lineage.IsValid() && frame.isInjectMode && frame.frameIndex == lineage.frameIndex &&
           frame.textureIndex == lineage.textureIndex && frame.fenceValue == lineage.fenceValue &&
           frame.ringIndex == lineage.ringIndex && frame.timestamp == lineage.timestamp;
}

bool MatchesInjectFrameLineage(const InjectFrameLineage& lhs, const InjectFrameLineage& rhs) {
    return lhs.IsValid() && rhs.IsValid() && lhs.frameIndex == rhs.frameIndex && lhs.textureIndex == rhs.textureIndex &&
           lhs.fenceValue == rhs.fenceValue && lhs.ringIndex == rhs.ringIndex && lhs.timestamp == rhs.timestamp;
}

bool IsInjectTextureIndexValid(int32_t textureIndex) {
    return textureIndex >= 0 && textureIndex < kInjectTextureSlotCount;
}
}  // namespace

static bool JoinThreadWithTimeout(std::thread& thread, DWORD timeoutMs, const char* threadName) {
    if (!thread.joinable()) {
        return true;
    }

    HANDLE threadHandle = reinterpret_cast<HANDLE>(thread.native_handle());
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

static void ReleaseStandaloneWgcQueuedFrame(QueuedFrame& frame) {
    if (!frame.isInjectMode && frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.wgcPoolLease.Reset();
    frame = QueuedFrame{};
}

static void ClearStandbyWgcHandoffFrame() {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
        if (!g_HasStandbyWgcFrame) {
            return;
        }
        stale = std::move(g_StandbyWgcFrame);
        g_HasStandbyWgcFrame = false;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
}

static bool HasStandbyWgcHandoffFrame() {
    std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
    return g_HasStandbyWgcFrame;
}

static bool StoreStandbyWgcHandoffFrame(QueuedFrame&& frame) {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
        // Recheck while holding the slot lock. A callback can observe the
        // retention flag immediately before the handoff thread disarms it; in
        // that race it must not repopulate the slot after the handoff has taken
        // the proven frame.
        if (!g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_HasStandbyWgcFrame) {
            stale = std::move(g_StandbyWgcFrame);
        }
        g_StandbyWgcFrame = std::move(frame);
        g_HasStandbyWgcFrame = true;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
    return true;
}

static bool TakeStandbyWgcHandoffFrame(QueuedFrame& frame) {
    std::lock_guard<std::mutex> lock(g_StandbyWgcFrameMutex);
    if (!g_HasStandbyWgcFrame) {
        return false;
    }
    frame = std::move(g_StandbyWgcFrame);
    g_HasStandbyWgcFrame = false;
    return true;
}

static void SubmitWgcQueuedFrame(QueuedFrame&& frame) {
    static std::atomic<int64_t> s_lastWgcTimestamp{0};
    if (g_pSharedMem) {
        const int64_t comparisonTimestamp = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
        const int64_t previousTimestamp = s_lastWgcTimestamp.exchange(comparisonTimestamp, std::memory_order_relaxed);
        if (previousTimestamp > 0) {
            if (comparisonTimestamp < previousTimestamp) {
                g_pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1, std::memory_order_relaxed);
            } else if (comparisonTimestamp == previousTimestamp) {
                g_pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        g_pSharedMem->runtimeState.sourceFramesReceived.fetch_add(1, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
    }

    // The queue unconditionally takes ownership of frame.texture, including on the
    // drop-oldest overflow path. Releasing a cached raw pointer here would double-release.
    g_FrameQueue.Push(std::move(frame));
}

static void QueueWgcCursorObservation(const ce::cursor::SourcePointerObservation& observation, int32_t captureLeft,
                                      int32_t captureTop, uint32_t captureWidth, uint32_t captureHeight,
                                      uint64_t sourceEpoch) {
    if (!observation.valid || observation.updateQpc <= 0 || captureWidth == 0 || captureHeight == 0 ||
        sourceEpoch != g_WgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }

    ce::cursor::CaptureState cursorState =
        CaptureCursorSnapshot(observation.updateQpc, captureLeft, captureTop, captureWidth, captureHeight, false);
    ce::cursor::ApplySourcePointerObservation(&cursorState, observation);
    std::lock_guard<std::mutex> lock(g_WgcCursorPublicationMutex);
    if (sourceEpoch != g_WgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }
    g_WgcCursorTimeline.Publish(cursorState);
    const uint64_t published = g_DxgiCursorTimelinePublished.fetch_add(1, std::memory_order_release) + 1;
    if (published == 1) {
        LogInfo(
            "[Cursor] DXGI QPC pointer timeline active: updateQpc=%lld position=(%d,%d) visible=%d embedded=%d "
            "coord=%s bounds=(%d,%d %ux%u)",
            static_cast<long long>(observation.updateQpc), observation.screenX, observation.screenY,
            observation.visible ? 1 : 0, observation.embedded ? 1 : 0,
            observation.positionIsShapeTopLeft ? "shape-top-left" : "hotspot", captureLeft, captureTop, captureWidth,
            captureHeight);
    }
}

static void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp,
                          int64_t rawTimestamp, bool isHDR, bool cursorEmbedded, bool duplicateSourceTimestamp,
                          const ce::cursor::SourcePointerObservation& cursorObservation, int32_t captureLeft,
                          int32_t captureTop, uint64_t sourceEpoch, WgcPoolSlotLease&& poolLease) {
    const uint64_t activeEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
    if (sourceEpoch != activeEpoch) {
        static std::atomic<uint64_t> s_staleEpochDrops{0};
        const uint64_t discarded = s_staleEpochDrops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (discarded <= 3 || (discarded % 120ull) == 0ull) {
            LogInfo("[WGC] Dropping retired-source callback frame: frameEpoch=%llu activeEpoch=%llu discarded=%llu",
                    static_cast<unsigned long long>(sourceEpoch), static_cast<unsigned long long>(activeEpoch),
                    static_cast<unsigned long long>(discarded));
        }
        if (texture) {
            texture->Release();
        }
        return;
    }
    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = texture;
    qf.width = width;
    qf.height = height;
    qf.timestamp = timestamp;
    qf.rawTimestamp = rawTimestamp;
    qf.selectionTimestamp = timestamp;
    qf.duplicateSourceTimestamp = duplicateSourceTimestamp;
    qf.wgcPoolSlot = poolLease.Slot();
    qf.wgcPoolGeneration = poolLease.Generation();
    qf.wgcSourceEpoch = sourceEpoch;
    qf.wgcPoolLease = std::move(poolLease);
    LARGE_INTEGER enqueueQpc;
    QueryPerformanceCounter(&enqueueQpc);
    qf.enqueueQpc = enqueueQpc.QuadPart;
    qf.isHDR = isHDR;
    qf.wgcCursorEmbedded = cursorEmbedded;
    qf.captureLeft = captureLeft;
    qf.captureTop = captureTop;
    qf.cursorState = CaptureCursorSnapshot(timestamp, captureLeft, captureTop, width, height, cursorEmbedded);
    ce::cursor::ApplySourcePointerObservation(&qf.cursorState, cursorObservation);
    g_WgcCursorTimeline.Publish(qf.cursorState);

    if (g_Recording.load(std::memory_order_acquire) && !IsActiveScreenGrab()) {
        if (g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire) &&
            StoreStandbyWgcHandoffFrame(std::move(qf))) {
            return;
        }
        const uint64_t discarded = g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (discarded <= 3 || (discarded % 120ull) == 0ull) {
            LogInfo(
                "[WGC] Dropping standby WGC frame while inject capture is active (discarded=%llu, ts=%lld). This "
                "prevents mid-recording encoder mode switches.",
                static_cast<unsigned long long>(discarded), static_cast<long long>(timestamp));
        }
        ReleaseStandaloneWgcQueuedFrame(qf);
        return;
    }

    SubmitWgcQueuedFrame(std::move(qf));
}

static QueuedFrame MakeQueuedWgcFrame(WGCCapturedFrame&& frame) {
    QueuedFrame qf;
    qf.isInjectMode = false;
    qf.texture = frame.texture;
    frame.texture = nullptr;
    qf.width = frame.width;
    qf.height = frame.height;
    qf.timestamp = frame.timestamp;
    qf.rawTimestamp = frame.rawTimestamp;
    qf.selectionTimestamp = frame.timestamp;
    qf.wgcPoolSlot = frame.poolSlot;
    qf.wgcPoolGeneration = frame.poolGeneration;
    qf.wgcSourceEpoch = frame.sourceEpoch;
    qf.wgcPoolLease = std::move(frame.poolLease);
    LARGE_INTEGER enqueueQpc;
    QueryPerformanceCounter(&enqueueQpc);
    qf.enqueueQpc = enqueueQpc.QuadPart;
    qf.isHDR = frame.isHDR;
    qf.wgcCursorEmbedded = frame.cursorEmbedded;
    qf.duplicateSourceTimestamp = frame.duplicateSourceTimestamp;
    qf.captureLeft = frame.captureLeft;
    qf.captureTop = frame.captureTop;
    qf.cursorState = CaptureCursorSnapshot(frame.timestamp, frame.captureLeft, frame.captureTop, frame.width,
                                           frame.height, frame.cursorEmbedded);
    ce::cursor::ApplySourcePointerObservation(&qf.cursorState, frame.cursorObservation);
    g_WgcCursorTimeline.Publish(qf.cursorState);
    return qf;
}

static void ReleaseWgcCapturedFrame(WGCCapturedFrame& frame) {
    if (frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.poolLease.Reset();
    frame.poolSlot = std::numeric_limits<uint32_t>::max();
    frame.poolGeneration = 0;
}

static void ResetInjectFrameRingToLatest(const char* reason) {
    if (!g_pSharedMem) {
        return;
    }

    FrameRingBuffer& ring = g_pSharedMem->frameRing;
    uint32_t readIndex = ring.readIndex.load(std::memory_order_acquire);
    uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);
    if (!IsFrameRingWindowValid(writeIndex, readIndex)) {
        LogError("[Media] Refusing corrupt inject frame ring reset before %s (write=%u read=%u distance=%u)",
                 reason ? reason : "unknown transition", writeIndex, readIndex,
                 static_cast<uint32_t>(writeIndex - readIndex));
        return;
    }
    if (readIndex == writeIndex) {
        return;
    }

    ring.readIndex.store(writeIndex, std::memory_order_release);
    ring.ingestIndex.store(writeIndex, std::memory_order_release);
    LogInfo("[Media] Discarded %u stale inject frame(s) before %s", static_cast<unsigned>(writeIndex - readIndex),
            reason);
}

static void ResetLastQueuedFrameCache() {
    if (g_HasLastFrame && !g_LastFrame.isInjectMode && g_LastFrame.texture) {
        g_LastFrame.texture->Release();
    }
    if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
        g_LastFrame.wgcPoolLease.Reset();
    }
    g_LastFrame = QueuedFrame{};
    g_HasLastFrame = false;
}

static bool EnsureInjectCaptureEvents() {
    if (!g_InjectCaptureShutdownEvent) {
        g_InjectCaptureShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_InjectCaptureShutdownEvent) {
            LogWarn("[Inject Thread] Failed to create shutdown event (err=%lu)", GetLastError());
        }
    }
    if (!g_InjectFrameReadyEvent && g_pSharedMem && g_pSharedMem->GetHostPID() != 0) {
        wchar_t eventName[64]{};
        GenerateInjectFrameReadyEventName(eventName, _countof(eventName), g_pSharedMem->GetHostPID());
        g_InjectFrameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (!g_InjectFrameReadyEvent) {
            LogWarn("[Inject Thread] Failed to create frame-ready event '%ls' (err=%lu)", eventName, GetLastError());
        } else {
            LogInfo("[Inject Thread] Frame-ready event initialized: %ls", eventName);
        }
    }
    return g_InjectFrameReadyEvent && g_InjectCaptureShutdownEvent;
}

static void StopInjectCapturePipeline() {
    g_InjectCaptureShutdown = true;
    if (g_InjectCaptureShutdownEvent) {
        SetEvent(g_InjectCaptureShutdownEvent);
    }
    JoinThreadWithTimeout(g_InjectCaptureThread, 5000, "inject capture");
    ResetInjectFrameRingToLatest("inject pipeline stop");
}

// Duplication embedded-cursor suppression: while duplicated frames already
// CONTAIN the cursor (software/composed cursor reported by the dup pointer
// metadata), encoder-side cursor composition must be suppressed to avoid a
// double cursor. Polled cheaply on the encoder thread per submitted frame;
// the state only changes on hardware/software cursor-plane transitions.
static std::atomic<bool> g_DupCursorSuppressionActive{false};

static void SyncDuplicationCursorSuppression(bool suppress) {
    if (suppress == g_DupCursorSuppressionActive.load(std::memory_order_relaxed)) {
        return;
    }
    g_DupCursorSuppressionActive.store(suppress, std::memory_order_relaxed);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        MediaEngine_SetCursorCompositionSuppressed(suppress);
    }
    LogInfo("[Media] Encoder cursor composition %s (duplication frames %s the cursor)",
            suppress ? "suppressed" : "active", suppress ? "already contain" : "do not contain");
}

static void ResetDuplicationCursorSuppression(const char* reason) {
    const bool wasSuppressed = g_DupCursorSuppressionActive.exchange(false, std::memory_order_acq_rel);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        // Always publish the reset. Merely clearing the local cache can leave
        // the encoder latched in suppression across a reset/retarget.
        MediaEngine_SetCursorCompositionSuppressed(false);
    }
    if (wasSuppressed) {
        LogInfo("[Media] Encoder cursor composition restored (%s)", reason ? reason : "capture transition");
    }
}

static void StopWgcCapturePipeline() {
    g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
    ClearStandbyWgcHandoffFrame();
    ResetDuplicationCursorSuppression("WGC pipeline stop");
    g_WgcCaptureShutdown = true;
    g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.wgcTargetFps.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    JoinThreadWithTimeout(g_WgcCaptureThread, 5000, "WGC capture");

    // Block encoder-side readers while the capture session and its WinRT/DXGI
    // resources are torn down. Atomic shared ownership alone protects object
    // lifetime; this access gate also protects mutable session internals.
    auto capture = g_WgcCap.LockExclusive();
    if (capture) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        capture->SetTargetFps(0);
        if (capture->IsCapturing()) {
            capture->StopCapture();
        }
    }
}

static bool StartWgcRecordingCapture(const AppConfig& config) {
    g_WgcRuntimeLogSnapshot.Reset();

    if (g_WgcCaptureThread.joinable()) {
        LogWarn("[Media] Cleaning up stale WGC capture thread before restart");
        g_WgcCaptureShutdown = true;
        JoinThreadWithTimeout(g_WgcCaptureThread, 5000, "WGC capture");
    }

    auto captureAccess = g_WgcCap.LockExclusive();
    WGCCapture* capture = captureAccess.get();
    if (!capture) {
        return false;
    }

    if (capture->IsCapturing()) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        capture->StopCapture();
    }

    capture->SetCaptureCursor(ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor));
    if (config.video.captureCursor) {
        LogInfo("[Media] WGC cursor capture: native WGC cursor disabled; encoder-side cursor composition enabled");
    }
    HWND activeWgcWindow = NULL;
    HMONITOR activeWgcMonitor = NULL;
    capture->GetTargetIdentity(&activeWgcWindow, &activeWgcMonitor);
    LogInfo(
        "[PrivacyBlackout] session enabled=%d target=%s policy=matching-foreground-fullscreen failMode=black "
        "processInspection=0 hooks=0",
        config.blackWhenNoFullscreenFocus ? 1 : 0,
        activeWgcWindow ? "window" : (activeWgcMonitor ? "monitor" : "unresolved"));
