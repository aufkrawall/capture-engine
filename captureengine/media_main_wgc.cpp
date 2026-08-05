#include "media_main_internal.h"

ce::cursor::CaptureState CaptureCursorSnapshot(int64_t associationQpc, int32_t captureLeft, int32_t captureTop,
                                                      uint32_t captureWidth, uint32_t captureHeight,
                                                      bool sourceEmbedded) {
    ce::cursor::CaptureState state;
    state.associationQpc = associationQpc;
    state.captureLeft = captureLeft;
    state.captureTop = captureTop;
    state.captureWidth = captureWidth;
    state.captureHeight = captureHeight;

    LARGE_INTEGER observedQpc;
    QueryPerformanceCounter(&observedQpc);
    state.observedQpc = observedQpc.QuadPart;

    CURSORINFO cursorInfo = {sizeof(CURSORINFO)};
    if (!GetCursorInfo(&cursorInfo)) {
        return state;
    }

    state.flags = ce::cursor::kStateValid;
    state.handle = reinterpret_cast<uint64_t>(cursorInfo.hCursor);
    state.screenX = cursorInfo.ptScreenPos.x;
    state.screenY = cursorInfo.ptScreenPos.y;
    if ((cursorInfo.flags & CURSOR_SUPPRESSED) != 0) {
        state.flags |= ce::cursor::kStateSuppressed;
    } else if ((cursorInfo.flags & CURSOR_SHOWING) != 0) {
        state.flags |= ce::cursor::kStateVisible;
    } else if (cursorInfo.hCursor) {
        // DirectFlip / independent-flip cursor planes can retain a valid
        // hardware cursor handle without CURSOR_SHOWING being observable in
        // this process. Preserve the existing compatibility fallback, but
        // record it so diagnostics can distinguish it from normal visibility.
        state.flags |= ce::cursor::kStateVisible | ce::cursor::kStateHandleVisibilityFallback;
    }
    state.SetSourceEmbedded(sourceEmbedded);

    state.dpi = GetCursorDpiAtPoint(cursorInfo.ptScreenPos);
    static thread_local UINT cachedMetricDpi = 0;
    static thread_local uint32_t cachedCursorWidth = 0;
    static thread_local uint32_t cachedCursorHeight = 0;
    if (state.dpi != cachedMetricDpi || cachedCursorWidth == 0 || cachedCursorHeight == 0) {
        cachedMetricDpi = state.dpi;
        cachedCursorWidth = static_cast<uint32_t>(std::max(1, GetCursorMetricForDpi(SM_CXCURSOR, state.dpi)));
        cachedCursorHeight = static_cast<uint32_t>(std::max(1, GetCursorMetricForDpi(SM_CYCURSOR, state.dpi)));
    }
    state.requestedWidth = cachedCursorWidth;
    state.requestedHeight = cachedCursorHeight;
    return state;
}

uint64_t AdvanceWgcSourceEpoch(const char* reason) {
    const uint64_t epoch = media_main_g_WgcSourceEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Cursor history is source-owned just like retained textures. Do not let a
    // newly published duplication source select exact-QPC samples from the
    // retired monitor/window epoch before its first pointer update arrives.
    {
        std::lock_guard<std::mutex> lock(media_main_g_WgcCursorPublicationMutex);
        media_main_g_WgcCursorTimeline.Clear();
        media_main_g_DxgiCursorTimelinePublished.store(0, std::memory_order_release);
    }
    LogInfo("[Media] Advanced WGC source epoch to %llu (%s)", static_cast<unsigned long long>(epoch),
            reason ? reason : "unspecified");
    return epoch;
}

void PublishWgcCapture(std::shared_ptr<WGCCapture> replacement, const char* reason) {
    const uint64_t epoch = AdvanceWgcSourceEpoch(reason);
    if (replacement) {
        // Bind the epoch before publication/start. A callback from the retired
        // source keeps its old identity even if it finishes after this global
        // coordinator epoch changes.
        replacement->SetSourceEpoch(epoch);
    }
    auto retired = media_main_g_WgcCap.Exchange(std::move(replacement));
    if (retired) {
        // Exchange holds the lifecycle writer gate until all reader expressions
        // finish. Releasing here keeps WinRT/COM teardown on the control thread
        // without retaining potentially large stopped texture pools all session.
        retired.reset();
    }
    LogInfo("[Media] Published WGC source epoch %llu", static_cast<unsigned long long>(epoch));
}

void SnapshotWgcRuntimeLogState(const WGCCapture* cap) {
    if (!cap) {
        return;
    }

    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.duplicateTimestampsSeen, cap->GetNormalizedDuplicateTimestampCount());
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.duplicateTimestampsSkipped, cap->GetDuplicateTimestampSkipCount());

    const uint32_t leasedMax = cap->GetPoolSlotLeasedMaxCount();
    const uint32_t freeMin = cap->GetPoolSlotFreeMinCount();
    if (leasedMax == 0 && freeMin == 0) {
        return;
    }

    media_main_g_WgcRuntimeLogSnapshot.sourceFramePoolBuffers.store(cap->GetSourceFramePoolBufferCount(),
                                                         std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.copyPoolSlots.store(cap->GetTexturePoolSlotCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.budgetSurfaces.store(cap->GetSmoothnessBudgetSurfaceCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.syncFrames.store(cap->GetSmoothnessSyncFrameCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.extraFrames.store(cap->GetSmoothnessRetainedFrameCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.retainedCap.store(cap->GetSmoothnessRetainedFrameCap(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.reservedFreeSlots.store(cap->GetSmoothnessReservedFreeSlotCount(),
                                                    std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.safetySlots.store(cap->GetSmoothnessSafetySlotCount(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.sourceFormat.store(cap->GetSmoothnessSourceDxgiFormat(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.retainedFormat.store(cap->GetSmoothnessCopyDxgiFormat(), std::memory_order_relaxed);
    const bool compact = cap->IsCompactRetainedCopyActive();
    media_main_g_WgcRuntimeLogSnapshot.compactRetained.store(compact ? 1u : 0u, std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.estimatedVramBytes.store(cap->GetSmoothnessEstimatedVramBytes(), std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.sourceBudgetBytes.store(cap->GetSmoothnessSourceEstimatedVramBytes(),
                                                    std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.copyBudgetBytes.store(cap->GetSmoothnessCopyEstimatedVramBytes(),
                                                  std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.sourceSurfaceBytes.store(cap->GetSmoothnessSourceBytesPerSurface(),
                                                     std::memory_order_relaxed);
    media_main_g_WgcRuntimeLogSnapshot.copySurfaceBytes.store(cap->GetSmoothnessCopyBytesPerSurface(), std::memory_order_relaxed);
    const int64_t convertUs = cap->GetLastPoolConvertTimeUs();
    if (compact || cap->IsUsingDesktopDuplication()) {
        if (convertUs > 0) {
            media_main_g_WgcRuntimeLogSnapshot.lastConvertUs.store(convertUs, std::memory_order_relaxed);
        }
    } else {
        media_main_g_WgcRuntimeLogSnapshot.lastConvertUs.store(0, std::memory_order_relaxed);
    }
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolLeasedMax, leasedMax);
    AtomicMin(media_main_g_WgcRuntimeLogSnapshot.poolFreeMin, freeMin);
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolSaturatedDrops, cap->GetPoolSaturatedDropCount());
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolOverwritePrevented, cap->GetPoolSlotOverwritePreventedCount());
    AtomicMax(media_main_g_WgcRuntimeLogSnapshot.poolLeaseMismatches, cap->GetPoolLeaseMismatchCount());
    media_main_g_WgcRuntimeLogSnapshot.hasPoolEvidence.store(true, std::memory_order_release);
}

void SnapshotPublishedWgcRuntimeLogState() {
    const auto cap = media_main_g_WgcCap.Read();
    SnapshotWgcRuntimeLogState(cap.get());
}

const char* WgcIngressAdmissionReasonName(uint32_t code) {
    switch (code) {
        case 1:
            return "low_water";
        case 2:
            return "recovery";
        case 3:
            return "source_below_cfr_target";
        case 4:
            return "credit";
        case 5:
            return "healthy";
        case 6:
            return "wgc_ingress_decimated_soft_reserve";
        case 7:
            return "wgc_ingress_decimated_hard_reserve";
        case 8:
            return "wgc_ingress_decimated_credit";
        case 9:
            return "uniform_playout_soft_reserve";
        case 10:
            return "uniform_playout_credit";
        case 0:
        default:
            return "uncapped";
    }
}

// Update g_IsEncoderBottlenecked with hysteresis to prevent rapid toggling.
// During startup we still learn the encode-time EMA, but we keep the
// bottleneck flag cleared so one-time encoder priming doesn't raise false
// overload warnings or skew WGC recovery logic.
void UpdateEncoderBottleneckFlag(double smoothedEncodeMs, double frameIntervalMs, bool startupWindowActive) {
    const bool currentlyBottlenecked = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
    bool newState = currentlyBottlenecked;
    if (startupWindowActive) {
        newState = false;
    } else if (currentlyBottlenecked) {
        // Exit bottleneck only when encode time drops well below the frame budget
        if (smoothedEncodeMs < frameIntervalMs * media_main_kBottleneckExitRatio) {
            newState = false;
        }
    } else {
        // Enter bottleneck when encode time approaches the frame budget
        if (smoothedEncodeMs > frameIntervalMs * media_main_kBottleneckEnterRatio) {
            newState = true;
        }
    }
    if (newState != currentlyBottlenecked) {
        media_main_g_IsEncoderBottlenecked.store(newState, std::memory_order_relaxed);
        if (media_main_g_pSharedMem) {
            media_main_g_pSharedMem->runtimeState.encoderBottlenecked.store(newState ? 1u : 0u, std::memory_order_relaxed);
        }
    }
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
    return textureIndex >= 0 && textureIndex < media_main_kInjectTextureSlotCount;
}

void ReleaseStandaloneWgcQueuedFrame(QueuedFrame& frame) {
    if (!frame.isInjectMode && frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.wgcPoolLease.Reset();
    frame = QueuedFrame{};
}

void ClearStandbyWgcHandoffFrame() {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
        if (!media_main_g_HasStandbyWgcFrame) {
            return;
        }
        stale = std::move(media_main_g_StandbyWgcFrame);
        media_main_g_HasStandbyWgcFrame = false;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
}

bool HasStandbyWgcHandoffFrame() {
    std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
    return media_main_g_HasStandbyWgcFrame;
}

bool StoreStandbyWgcHandoffFrame(QueuedFrame&& frame) {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
        // Recheck while holding the slot lock. A callback can observe the
        // retention flag immediately before the handoff thread disarms it; in
        // that race it must not repopulate the slot after the handoff has taken
        // the proven frame.
        if (!media_main_g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire)) {
            return false;
        }
        if (media_main_g_HasStandbyWgcFrame) {
            stale = std::move(media_main_g_StandbyWgcFrame);
        }
        media_main_g_StandbyWgcFrame = std::move(frame);
        media_main_g_HasStandbyWgcFrame = true;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
    return true;
}

bool TakeStandbyWgcHandoffFrame(QueuedFrame& frame) {
    std::lock_guard<std::mutex> lock(media_main_g_StandbyWgcFrameMutex);
    if (!media_main_g_HasStandbyWgcFrame) {
        return false;
    }
    frame = std::move(media_main_g_StandbyWgcFrame);
    media_main_g_HasStandbyWgcFrame = false;
    return true;
}

void SubmitWgcQueuedFrame(QueuedFrame&& frame) {
    static std::atomic<int64_t> s_lastWgcTimestamp{0};
    if (media_main_g_pSharedMem) {
        const int64_t comparisonTimestamp = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
        const int64_t previousTimestamp = s_lastWgcTimestamp.exchange(comparisonTimestamp, std::memory_order_relaxed);
        if (previousTimestamp > 0) {
            if (comparisonTimestamp < previousTimestamp) {
                media_main_g_pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1, std::memory_order_relaxed);
            } else if (comparisonTimestamp == previousTimestamp) {
                media_main_g_pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        media_main_g_pSharedMem->runtimeState.sourceFramesReceived.fetch_add(1, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
    }

    // The queue unconditionally takes ownership of frame.texture, including on the
    // drop-oldest overflow path. Releasing a cached raw pointer here would double-release.
    media_main_g_FrameQueue.Push(std::move(frame));
}

void QueueWgcCursorObservation(const ce::cursor::SourcePointerObservation& observation, int32_t captureLeft,
                                      int32_t captureTop, uint32_t captureWidth, uint32_t captureHeight,
                                      uint64_t sourceEpoch) {
    if (!observation.valid || observation.updateQpc <= 0 || captureWidth == 0 || captureHeight == 0 ||
        sourceEpoch != media_main_g_WgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }

    ce::cursor::CaptureState cursorState =
        CaptureCursorSnapshot(observation.updateQpc, captureLeft, captureTop, captureWidth, captureHeight, false);
    ce::cursor::ApplySourcePointerObservation(&cursorState, observation);
    std::lock_guard<std::mutex> lock(media_main_g_WgcCursorPublicationMutex);
    if (sourceEpoch != media_main_g_WgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }
    media_main_g_WgcCursorTimeline.Publish(cursorState);
    const uint64_t published = media_main_g_DxgiCursorTimelinePublished.fetch_add(1, std::memory_order_release) + 1;
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

void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp,
                          int64_t rawTimestamp, bool isHDR, bool cursorEmbedded, bool duplicateSourceTimestamp,
                          const ce::cursor::SourcePointerObservation& cursorObservation, int32_t captureLeft,
                          int32_t captureTop, uint64_t sourceEpoch, WgcPoolSlotLease&& poolLease) {
    const uint64_t activeEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
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
    media_main_g_WgcCursorTimeline.Publish(qf.cursorState);

    if (media_main_g_Recording.load(std::memory_order_acquire) && !IsActiveScreenGrab()) {
        if (media_main_g_RetainStandbyWgcFrameForHandoff.load(std::memory_order_acquire) &&
            StoreStandbyWgcHandoffFrame(std::move(qf))) {
            return;
        }
        const uint64_t discarded = media_main_g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
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

QueuedFrame MakeQueuedWgcFrame(WGCCapturedFrame&& frame) {
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
    media_main_g_WgcCursorTimeline.Publish(qf.cursorState);
    return qf;
}

void ReleaseWgcCapturedFrame(WGCCapturedFrame& frame) {
    if (frame.texture) {
        frame.texture->Release();
        frame.texture = nullptr;
    }
    frame.poolLease.Reset();
    frame.poolSlot = std::numeric_limits<uint32_t>::max();
    frame.poolGeneration = 0;
}

void ResetInjectFrameRingToLatest(const char* reason) {
    if (!media_main_g_pSharedMem) {
        return;
    }

    FrameRingBuffer& ring = media_main_g_pSharedMem->frameRing;
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

void ResetLastQueuedFrameCache() {
    if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode && media_main_g_LastFrame.texture) {
        media_main_g_LastFrame.texture->Release();
    }
    if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode) {
        media_main_g_LastFrame.wgcPoolLease.Reset();
    }
    media_main_g_LastFrame = QueuedFrame{};
    media_main_g_HasLastFrame = false;
}

bool EnsureInjectCaptureEvents() {
    if (!media_main_g_InjectCaptureShutdownEvent) {
        media_main_g_InjectCaptureShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!media_main_g_InjectCaptureShutdownEvent) {
            LogWarn("[Inject Thread] Failed to create shutdown event (err=%lu)", GetLastError());
        }
    }
    if (!media_main_g_InjectFrameReadyEvent && media_main_g_pSharedMem && media_main_g_pSharedMem->GetHostPID() != 0) {
        wchar_t eventName[64]{};
        GenerateInjectFrameReadyEventName(eventName, _countof(eventName), media_main_g_pSharedMem->GetHostPID());
        media_main_g_InjectFrameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (!media_main_g_InjectFrameReadyEvent) {
            LogWarn("[Inject Thread] Failed to create frame-ready event '%ls' (err=%lu)", eventName, GetLastError());
        } else {
            LogInfo("[Inject Thread] Frame-ready event initialized: %ls", eventName);
        }
    }
    return media_main_g_InjectFrameReadyEvent && media_main_g_InjectCaptureShutdownEvent;
}

void StopInjectCapturePipeline() {
    media_main_g_InjectCaptureShutdown = true;
    if (media_main_g_InjectCaptureShutdownEvent) {
        SetEvent(media_main_g_InjectCaptureShutdownEvent);
    }
    JoinThreadWithTimeout(media_main_g_InjectCaptureThread, 5000, "inject capture");
    ResetInjectFrameRingToLatest("inject pipeline stop");
}

void SyncDuplicationCursorSuppression(bool suppress) {
    if (suppress == media_main_g_DupCursorSuppressionActive.load(std::memory_order_relaxed)) {
        return;
    }
    media_main_g_DupCursorSuppressionActive.store(suppress, std::memory_order_relaxed);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        MediaEngine_SetCursorCompositionSuppressed(suppress);
    }
    LogInfo("[Media] Encoder cursor composition %s (duplication frames %s the cursor)",
            suppress ? "suppressed" : "active", suppress ? "already contain" : "do not contain");
}

void ResetDuplicationCursorSuppression(const char* reason) {
    const bool wasSuppressed = media_main_g_DupCursorSuppressionActive.exchange(false, std::memory_order_acq_rel);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        // Always publish the reset. Merely clearing the local cache can leave
        // the encoder latched in suppression across a reset/retarget.
        MediaEngine_SetCursorCompositionSuppressed(false);
    }
    if (wasSuppressed) {
        LogInfo("[Media] Encoder cursor composition restored (%s)", reason ? reason : "capture transition");
    }
}

void StopWgcCapturePipeline() {
    media_main_g_RetainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
    ClearStandbyWgcHandoffFrame();
    ResetDuplicationCursorSuppression("WGC pipeline stop");
    media_main_g_WgcCaptureShutdown = true;
    media_main_g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.wgcTargetFps.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    JoinThreadWithTimeout(media_main_g_WgcCaptureThread, 5000, "WGC capture");

    // Block encoder-side readers while the capture session and its WinRT/DXGI
    // resources are torn down. Atomic shared ownership alone protects object
    // lifetime; this access gate also protects mutable session internals.
    auto capture = media_main_g_WgcCap.LockExclusive();
    if (capture) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        capture->SetTargetFps(0);
        if (capture->IsCapturing()) {
            capture->StopCapture();
        }
    }
}

bool StartWgcRecordingCapture(const AppConfig& config) {
    media_main_g_WgcRuntimeLogSnapshot.Reset();

    if (media_main_g_WgcCaptureThread.joinable()) {
        LogWarn("[Media] Cleaning up stale WGC capture thread before restart");
        media_main_g_WgcCaptureShutdown = true;
        JoinThreadWithTimeout(media_main_g_WgcCaptureThread, 5000, "WGC capture");
    }

    auto captureAccess = media_main_g_WgcCap.LockExclusive();
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

    int32_t activeCaptureLeft = 0;
    int32_t activeCaptureTop = 0;
    const bool haveCaptureOrigin = capture->GetCaptureOrigin(activeCaptureLeft, activeCaptureTop);
    LogInfo(
        "[Media] WGC recording target: target=%s backend=%s hwnd=0x%p hmon=0x%p originOk=%d origin=(%d,%d) "
        "captureCursor=%d nativeWgcCursor=%d encoderCursor=%d",
        activeWgcWindow ? "window" : "monitor", capture->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
        activeWgcWindow, activeWgcMonitor, haveCaptureOrigin ? 1 : 0, activeCaptureLeft, activeCaptureTop,
        config.video.captureCursor ? 1 : 0,
        ce::capture_policy::ShouldUseNativeWgcCursorCapture(config.video.captureCursor) ? 1 : 0,
        config.video.captureCursor ? 1 : 0);
    capture->SetSkipSplitDeviceFlush(config.wgcSkipSplitDeviceFlush);
    capture->SetSameDeviceCapture(config.wgcSameDeviceCapture);
    capture->SetAllowLossyBgra8Pool(config.wgcAllowLossyBgra8Pool);
    const bool explicitTenBit = IsExplicitTenBitVideo(config.video);
    capture->SetRequireHighPrecisionCapture(explicitTenBit);
    capture->SetAllowDuplicationFallback(ce::capture_policy::ShouldAllowWgcFallbackAfterDxgiFailure(
        IsDxgiDupCaptureMethod(config.captureMethod), explicitTenBit));
    const uint32_t initialWgcTargetFps = GetInitialWgcCfrTargetFps(config.video);
    float maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    const uint32_t outputFps = static_cast<uint32_t>(std::max(0, config.video.fps));
    const bool hasWgcContentDelayBudget = maxAudioCaptureLatencyMs > 0.0f;
    // Smoothness FLOOR: when configured (auto or explicit > 0) the reservoir/copy-pool budget must
    // be allocated even with no audio-latency content delay, otherwise a video-only / low-confidence
    // capture would have no buffer to engage the active-delay jitter-absorbing playout. The floor
    // delay itself is realized within the retained-extra reservoir (not the sync-delay frames), so
    // syncDelayFramesForBudget stays audio-latency-driven (0 here when there is no audio latency).
    const bool wgcSmoothnessFloorBudgetDesired = config.wgcSmoothnessBufferEnabled && !config.video.useVFR &&
                                                 (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    const uint32_t syncDelayFramesForBudget =
        hasWgcContentDelayBudget ? ce::capture_policy::GetWgcEstimatedSyncDelayFramesForBudget(
                                       outputFps, static_cast<uint32_t>(std::ceil(maxAudioCaptureLatencyMs)))
                                 : 0u;
    capture->SetSmoothnessBufferBudget(config.wgcSmoothnessBufferEnabled && !config.video.useVFR &&
                                           (hasWgcContentDelayBudget || wgcSmoothnessFloorBudgetDesired),
                                       outputFps, config.wgcSmoothnessBufferMaxMs,
                                       config.wgcSmoothnessBufferVramBudgetMb, syncDelayFramesForBudget);
    capture->SetVideoMemoryReservationMode(config.wgcVideoMemoryReservation);
    if (config.video.useVFR) {
        capture->SetDirectFrameCallback(QueueWgcFrame);
    } else {
        capture->SetDirectFrameCallback(nullptr);
    }
    capture->SetDirectCursorCallback(config.video.captureCursor ? QueueWgcCursorObservation : nullptr);
    capture->ResetStats();
    // Explicitly reset both the cache and the encoder-side state. A prior
    // duplication session may have ended while its software cursor was embedded.
    ResetDuplicationCursorSuppression("WGC recording start");
    media_main_g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    media_main_g_WgcProducerTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
    // A finite WGC MinUpdateInterval aliases variable-rate sources and can turn
    // 138 fps into about 69 fps. CFR therefore receives every compositor update
    // and leaves surplus-frame selection to the timestamp scheduler. DXGI
    // duplication has no producer interval; it shares the same zero target so
    // the screen-grab contract is backend-independent.
    capture->SetTargetFps(0);
    capture->SetProducerTargetFps(initialWgcTargetFps);
    LogInfo(
        "[WGC CFR] Producer contract: backend=%s outputFps=%u producerTargetFps=%u minUpdateInterval100ns=0 "
        "policy=max-rate-variable-input localThrottleFps=0",
        capture->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc", outputFps, initialWgcTargetFps);
    // Persist before StartCapture so any device rebuild and the first WGC/DXGI
    // submissions inherit the configured relative GPU priority.
    capture->SetGpuPriority(config.video.gpuPriority);

    // For CFR recording, disable the encoder-bottleneck throttle at the WGC
    // callback level.  The throttle is all-or-nothing (bang-bang) and its slow
    // EMA causes boom-bust oscillation that starves the Bresenham credit
    // accumulator, producing irregular frame-hold patterns (visible judder).
    // The encoder thread's buffer cap + Bresenham skip already provide smooth
    // backpressure, so the throttle is both unnecessary and harmful for CFR.
    if (!config.video.useVFR) {
        capture->SetThrottleFlag(nullptr);
        LogInfo("[Media] WGC CFR mode: pull-latest sampling enabled, callback queue bypassed");
        LogInfo("[Media] WGC CFR mode: encoder-bottleneck throttle disabled (buffer cap provides backpressure)");
    }
    if (media_main_g_pSharedMem) {
        media_main_g_pSharedMem->runtimeState.wgcTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    if (!capture->StartCapture()) {
        capture->SetDirectFrameCallback(nullptr);
        capture->SetDirectCursorCallback(nullptr);
        return false;
    }
    SnapshotWgcRuntimeLogState(capture);

    // Tell the encoder whether the capture source runs at >8 bpc so that
    // bit_depth=auto resolves to 10-bit even when the WGC frame pool fell
    // back to BGRA8 (e.g. R10G10B10A2 pool creation failed).
    if (MediaEngine_SetSourcePrefers10Bit) {
        const bool hiPrec = capture->IsHighPrecisionSource();
        LogInfo("[Media] WGC source high-precision=%s, notifying encoder", hiPrec ? "YES" : "NO");
        MediaEngine_SetSourcePrefers10Bit(hiPrec);
    } else {
        LogWarn("[Media] MediaEngine_SetSourcePrefers10Bit not available (old mediaengine.dll?)");
    }

    media_main_g_WgcCaptureShutdown = false;
    // Recording-lifetime config snapshot: the main thread reassigns `config`
    // on late hook connects and IPC config reloads (refreshActiveConfig),
    // which would be a use-after-free race against a by-reference reader on
    // this thread. Recording settings must not change live mid-session anyway.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        media_main_g_WgcCaptureThread = std::thread([configSnapshot]() { WgcCaptureThreadFunc(*configSnapshot); });
    }
    return true;
}
