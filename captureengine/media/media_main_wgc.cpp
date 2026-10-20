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

    CURSORINFO cursorInfo = {};
    cursorInfo.cbSize = sizeof(cursorInfo);
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
    const uint64_t epoch = MediaState().wgcSourceEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Cursor history is source-owned just like retained textures. Do not let a
    // newly published duplication source select exact-QPC samples from the
    // retired monitor/window epoch before its first pointer update arrives.
    {
        std::lock_guard<std::mutex> lock(MediaState().wgcCursorPublicationMutex);
        MediaState().wgcCursorTimeline.Clear();
        MediaState().dxgiCursorTimelinePublished.store(0, std::memory_order_release);
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
    auto retired = MediaState().wgcCap.Exchange(std::move(replacement));
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

    AtomicMax(MediaState().wgcRuntimeLogSnapshot.duplicateTimestampsSeen, cap->GetNormalizedDuplicateTimestampCount());
    AtomicMax(MediaState().wgcRuntimeLogSnapshot.duplicateTimestampsSkipped, cap->GetDuplicateTimestampSkipCount());

    const uint32_t leasedMax = cap->GetPoolSlotLeasedMaxCount();
    const uint32_t freeMin = cap->GetPoolSlotFreeMinCount();
    if (leasedMax == 0 && freeMin == 0) {
        return;
    }

    MediaState().wgcRuntimeLogSnapshot.sourceFramePoolBuffers.store(cap->GetSourceFramePoolBufferCount(),
                                                         std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.copyPoolSlots.store(cap->GetTexturePoolSlotCount(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.budgetSurfaces.store(cap->GetSmoothnessBudgetSurfaceCount(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.syncFrames.store(cap->GetSmoothnessSyncFrameCount(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.extraFrames.store(cap->GetSmoothnessRetainedFrameCount(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.retainedCap.store(cap->GetSmoothnessRetainedFrameCap(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.reservedFreeSlots.store(cap->GetSmoothnessReservedFreeSlotCount(),
                                                    std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.safetySlots.store(cap->GetSmoothnessSafetySlotCount(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.sourceFormat.store(cap->GetSmoothnessSourceDxgiFormat(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.retainedFormat.store(cap->GetSmoothnessCopyDxgiFormat(), std::memory_order_relaxed);
    const bool compact = cap->IsCompactRetainedCopyActive();
    MediaState().wgcRuntimeLogSnapshot.compactRetained.store(compact ? 1u : 0u, std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.estimatedVramBytes.store(cap->GetSmoothnessEstimatedVramBytes(), std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.sourceBudgetBytes.store(cap->GetSmoothnessSourceEstimatedVramBytes(),
                                                    std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.copyBudgetBytes.store(cap->GetSmoothnessCopyEstimatedVramBytes(),
                                                  std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.sourceSurfaceBytes.store(cap->GetSmoothnessSourceBytesPerSurface(),
                                                     std::memory_order_relaxed);
    MediaState().wgcRuntimeLogSnapshot.copySurfaceBytes.store(cap->GetSmoothnessCopyBytesPerSurface(), std::memory_order_relaxed);
    const int64_t convertUs = cap->GetLastPoolConvertTimeUs();
    if (compact || cap->IsUsingDesktopDuplication()) {
        if (convertUs > 0) {
            MediaState().wgcRuntimeLogSnapshot.lastConvertUs.store(convertUs, std::memory_order_relaxed);
        }
    } else {
        MediaState().wgcRuntimeLogSnapshot.lastConvertUs.store(0, std::memory_order_relaxed);
    }
    AtomicMax(MediaState().wgcRuntimeLogSnapshot.poolLeasedMax, leasedMax);
    AtomicMin(MediaState().wgcRuntimeLogSnapshot.poolFreeMin, freeMin);
    AtomicMax(MediaState().wgcRuntimeLogSnapshot.poolSaturatedDrops, cap->GetPoolSaturatedDropCount());
    AtomicMax(MediaState().wgcRuntimeLogSnapshot.poolOverwritePrevented, cap->GetPoolSlotOverwritePreventedCount());
    AtomicMax(MediaState().wgcRuntimeLogSnapshot.poolLeaseMismatches, cap->GetPoolLeaseMismatchCount());
    MediaState().wgcRuntimeLogSnapshot.hasPoolEvidence.store(true, std::memory_order_release);
}

void SnapshotPublishedWgcRuntimeLogState() {
    const auto cap = MediaState().wgcCap.Read();
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
    const bool currentlyBottlenecked = MediaState().isEncoderBottlenecked.load(std::memory_order_relaxed);
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
        MediaState().isEncoderBottlenecked.store(newState, std::memory_order_relaxed);
        if (MediaState().pSharedMem) {
            MediaState().pSharedMem->runtimeState.encoderBottlenecked.store(newState ? 1u : 0u, std::memory_order_relaxed);
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
        std::lock_guard<std::mutex> lock(MediaState().standbyWgcFrameMutex);
        if (!MediaState().hasStandbyWgcFrame) {
            return;
        }
        stale = std::move(MediaState().standbyWgcFrame);
        MediaState().hasStandbyWgcFrame = false;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
}

bool HasStandbyWgcHandoffFrame() {
    std::lock_guard<std::mutex> lock(MediaState().standbyWgcFrameMutex);
    return MediaState().hasStandbyWgcFrame;
}

bool StoreStandbyWgcHandoffFrame(QueuedFrame&& frame) {
    QueuedFrame stale;
    {
        std::lock_guard<std::mutex> lock(MediaState().standbyWgcFrameMutex);
        // Recheck while holding the slot lock. A callback can observe the
        // retention flag immediately before the handoff thread disarms it; in
        // that race it must not repopulate the slot after the handoff has taken
        // the proven frame.
        if (!MediaState().retainStandbyWgcFrameForHandoff.load(std::memory_order_acquire)) {
            return false;
        }
        if (MediaState().hasStandbyWgcFrame) {
            stale = std::move(MediaState().standbyWgcFrame);
        }
        MediaState().standbyWgcFrame = std::move(frame);
        MediaState().hasStandbyWgcFrame = true;
    }
    ReleaseStandaloneWgcQueuedFrame(stale);
    return true;
}

bool TakeStandbyWgcHandoffFrame(QueuedFrame& frame) {
    std::lock_guard<std::mutex> lock(MediaState().standbyWgcFrameMutex);
    if (!MediaState().hasStandbyWgcFrame) {
        return false;
    }
    frame = std::move(MediaState().standbyWgcFrame);
    MediaState().hasStandbyWgcFrame = false;
    return true;
}

void SubmitWgcQueuedFrame(QueuedFrame&& frame) {
    if (MediaState().pSharedMem) {
        const int64_t comparisonTimestamp = frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
        const int64_t previousTimestamp = MediaState().lastWgcTimestamp.exchange(comparisonTimestamp, std::memory_order_relaxed);
        if (previousTimestamp > 0) {
            if (comparisonTimestamp < previousTimestamp) {
                MediaState().pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1, std::memory_order_relaxed);
            } else if (comparisonTimestamp == previousTimestamp) {
                MediaState().pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        MediaState().pSharedMem->runtimeState.sourceFramesReceived.fetch_add(1, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
    }

    // The queue unconditionally takes ownership of frame.texture, including on the
    // drop-oldest overflow path. Releasing a cached raw pointer here would double-release.
    MediaState().frameQueue.Push(std::move(frame));
}

void QueueWgcCursorObservation(const ce::cursor::SourcePointerObservation& observation, int32_t captureLeft,
                                      int32_t captureTop, uint32_t captureWidth, uint32_t captureHeight,
                                      uint64_t sourceEpoch) {
    if (!observation.valid || observation.updateQpc <= 0 || captureWidth == 0 || captureHeight == 0 ||
        sourceEpoch != MediaState().wgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }

    ce::cursor::CaptureState cursorState =
        CaptureCursorSnapshot(observation.updateQpc, captureLeft, captureTop, captureWidth, captureHeight, false);
    ce::cursor::ApplySourcePointerObservation(&cursorState, observation);
    std::lock_guard<std::mutex> lock(MediaState().wgcCursorPublicationMutex);
    if (sourceEpoch != MediaState().wgcSourceEpoch.load(std::memory_order_acquire)) {
        return;
    }
    MediaState().wgcCursorTimeline.Publish(cursorState);
    const uint64_t published = MediaState().dxgiCursorTimelinePublished.fetch_add(1, std::memory_order_release) + 1;
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
                          int32_t captureTop, uint64_t sourceEpoch, WgcPoolSlotLease&& poolLease,
                          uint32_t sourceFrameSpan) {
    const uint64_t activeEpoch = MediaState().wgcSourceEpoch.load(std::memory_order_acquire);
    if (sourceEpoch != activeEpoch) {
        const uint64_t discarded = MediaState().staleEpochDrops.fetch_add(1, std::memory_order_relaxed) + 1;
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
    qf.sourceFrameSpan = sourceFrameSpan > 0 ? sourceFrameSpan : 1u;
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
    MediaState().wgcCursorTimeline.Publish(qf.cursorState);

    if (MediaState().recording.load(std::memory_order_acquire) && !IsActiveScreenGrab()) {
        if (MediaState().retainStandbyWgcFrameForHandoff.load(std::memory_order_acquire) &&
            StoreStandbyWgcHandoffFrame(std::move(qf))) {
            return;
        }
        const uint64_t discarded = MediaState().activePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
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
    qf.sourceFrameSpan = frame.sourceFrameSpan > 0 ? frame.sourceFrameSpan : 1u;
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
    MediaState().wgcCursorTimeline.Publish(qf.cursorState);
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
    if (!MediaState().pSharedMem) {
        return;
    }

    FrameRingBuffer& ring = MediaState().pSharedMem->frameRing;
    uint32_t discarded = 0;
    if (!ce::DiscardUnreadInjectFrameRingSlots(ring, discarded)) {
        LogError("[Media] Refusing corrupt inject frame ring reset before %s (write=%u ingest=%u read=%u)",
                 reason ? reason : "unknown transition", ring.writeIndex.load(std::memory_order_acquire),
                 ring.ingestIndex.load(std::memory_order_acquire),
                 ring.readIndex.load(std::memory_order_acquire));
        return;
    }
    const size_t pendingReads = MediaState().injectSourceReadReleases.Pending();
    if (discarded != 0 || pendingReads != 0) {
        LogInfo("[Inject SourceRead] Ring restart before %s: discarded=%u ingest=%u read=%u write=%u pendingReads=%zu",
                reason ? reason : "unknown transition", discarded, ring.ingestIndex.load(std::memory_order_acquire),
                ring.readIndex.load(std::memory_order_acquire), ring.writeIndex.load(std::memory_order_acquire),
                pendingReads);
    }
}

void ResetLastQueuedFrameCache() {
    if (MediaState().hasLastFrame && !MediaState().lastFrame.isInjectMode && MediaState().lastFrame.texture) {
        MediaState().lastFrame.texture->Release();
    }
    if (MediaState().hasLastFrame && !MediaState().lastFrame.isInjectMode) {
        MediaState().lastFrame.wgcPoolLease.Reset();
    }
    MediaState().lastFrame = QueuedFrame{};
    MediaState().hasLastFrame = false;
}

bool EnsureInjectCaptureEvents() {
    if (!MediaState().injectCaptureShutdownEvent) {
        MediaState().injectCaptureShutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!MediaState().injectCaptureShutdownEvent) {
            LogWarn("[Inject Thread] Failed to create shutdown event (err=%lu)", GetLastError());
        }
    }
    if (!MediaState().injectFrameReadyEvent && MediaState().pSharedMem && MediaState().pSharedMem->GetHostPID() != 0) {
        wchar_t eventName[64]{};
        GenerateInjectFrameReadyEventName(eventName, _countof(eventName), MediaState().pSharedMem->GetHostPID());
        MediaState().injectFrameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, eventName);
        if (!MediaState().injectFrameReadyEvent) {
            LogWarn("[Inject Thread] Failed to create frame-ready event '%ls' (err=%lu)", eventName, GetLastError());
        } else {
            LogInfo("[Inject Thread] Frame-ready event initialized: %ls", eventName);
        }
    }
    return MediaState().injectFrameReadyEvent && MediaState().injectCaptureShutdownEvent;
}

void StopInjectCapturePipeline() {
    MediaState().injectCaptureShutdown = true;
    if (MediaState().injectCaptureShutdownEvent) {
        SetEvent(MediaState().injectCaptureShutdownEvent);
    }
    JoinThreadWithTimeout(MediaState().injectCaptureThread, 5000, "inject capture");
}

void SyncDuplicationCursorSuppression(bool suppress) {
    if (suppress == MediaState().dupCursorSuppressionActive.load(std::memory_order_relaxed)) {
        return;
    }
    MediaState().dupCursorSuppressionActive.store(suppress, std::memory_order_relaxed);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        MediaEngine_SetCursorCompositionSuppressed(MediaState().engine, suppress);
    }
    LogInfo("[Media] Encoder cursor composition %s (duplication frames %s the cursor)",
            suppress ? "suppressed" : "active", suppress ? "already contain" : "do not contain");
}

void ResetDuplicationCursorSuppression(const char* reason) {
    const bool wasSuppressed = MediaState().dupCursorSuppressionActive.exchange(false, std::memory_order_acq_rel);
    if (MediaEngine_SetCursorCompositionSuppressed) {
        // Always publish the reset. Merely clearing the local cache can leave
        // the encoder latched in suppression across a reset/retarget.
        MediaEngine_SetCursorCompositionSuppressed(MediaState().engine, false);
    }
    if (wasSuppressed) {
        LogInfo("[Media] Encoder cursor composition restored (%s)", reason ? reason : "capture transition");
    }
}

void StopWgcCapturePipeline() {
    MediaState().retainStandbyWgcFrameForHandoff.store(false, std::memory_order_release);
    ClearStandbyWgcHandoffFrame();
    ResetDuplicationCursorSuppression("WGC pipeline stop");
    MediaState().wgcCaptureShutdown = true;
    MediaState().wgcProducerTargetFps.store(0, std::memory_order_relaxed);
    if (MediaState().pSharedMem) {
        MediaState().pSharedMem->runtimeState.wgcTargetFps.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
    }
    JoinThreadWithTimeout(MediaState().wgcCaptureThread, 5000, "WGC capture");

    // Block encoder-side readers while the capture session and its WinRT/DXGI
    // resources are torn down. Atomic shared ownership alone protects object
    // lifetime; this access gate also protects mutable session internals.
    auto capture = MediaState().wgcCap.LockExclusive();
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
    MediaState().wgcRuntimeLogSnapshot.Reset();

    if (MediaState().wgcCaptureThread.joinable()) {
        LogWarn("[Media] Cleaning up stale WGC capture thread before restart");
        MediaState().wgcCaptureShutdown = true;
        JoinThreadWithTimeout(MediaState().wgcCaptureThread, 5000, "WGC capture");
    }

    auto captureAccess = MediaState().wgcCap.LockExclusive();
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
    capture->SetRetainedYuvEnabled(config.wgcRetainedYuv);
    capture->SetReportsGpuMemory(true);
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
    MediaState().isEncoderBottlenecked.store(false, std::memory_order_relaxed);
    MediaState().wgcProducerTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
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
    if (MediaState().pSharedMem) {
        MediaState().pSharedMem->runtimeState.wgcTargetFps.store(initialWgcTargetFps, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcCaptureHealthFlags.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcCaptureHealthFps.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionErrorAvgUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionErrorMaxUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionEarlyMaxUs.store(0, std::memory_order_relaxed);
        MediaState().pSharedMem->runtimeState.wgcSelectionLateMaxUs.store(0, std::memory_order_relaxed);
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
        MediaEngine_SetSourcePrefers10Bit(MediaState().engine, hiPrec);
    } else {
        LogWarn("[Media] MediaEngine_SetSourcePrefers10Bit not available (old mediaengine.dll?)");
    }

    MediaState().wgcCaptureShutdown = false;
    // Recording-lifetime config snapshot: the main thread reassigns `config`
    // on late hook connects and IPC config reloads (refreshActiveConfig),
    // which would be a use-after-free race against a by-reference reader on
    // this thread. Recording settings must not change live mid-session anyway.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        MediaSessionState* session = &MediaState();
        MediaState().wgcCaptureThread = std::thread([configSnapshot, session]() {
            MediaSessionBinding binding(*session);
            WgcCaptureThreadFunc(*configSnapshot);
        });
    }
    return true;
}
