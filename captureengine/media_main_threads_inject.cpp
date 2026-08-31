#include "media_main_internal.h"

void InjectCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[Inject Thread] Started (event-driven ingest with adaptive source-side pacing)");
    media_main_g_InjectCaptureRunning = true;
    DisableCurrentThreadPowerThrottling("Inject Thread");
    ScopedMmcssTask injectMmcssTask(L"Capture", AVRT_PRIORITY_HIGH, "Inject Thread");

    if (!media_main_g_pSharedMem) {
        LogError("[Inject Thread] Shared memory not available! Aborting.");
        media_main_g_InjectCaptureRunning = false;
        return;
    }

    LUID lastSchedulingLuid{};
    bool haveSchedulingLuid = false;
    const auto applySchedulingForCurrentAdapter = [&]() {
        LUID current{};
        current.LowPart = media_main_g_pSharedMem->GetLuidLowPart();
        current.HighPart = static_cast<LONG>(media_main_g_pSharedMem->GetLuidHighPart());
        if (current.LowPart == 0 && current.HighPart == 0) {
            return;
        }
        if (!haveSchedulingLuid || !ce::windows_gpu_scheduling::SameLuid(lastSchedulingLuid, current)) {
            ApplyMediaGpuSchedulingPriority(config, &current);
            lastSchedulingLuid = current;
            haveSchedulingLuid = true;
        }
    };
    applySchedulingForCurrentAdapter();

    // Local read index tracks what WE have pushed to the FrameQueue
    uint32_t localReadIndex = media_main_g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
    media_main_g_pSharedMem->frameRing.ingestIndex.store(localReadIndex, std::memory_order_release);
    auto advanceIngestIndex = [&]() {
        ++localReadIndex;
        media_main_g_pSharedMem->frameRing.ingestIndex.store(localReadIndex, std::memory_order_release);
    };
    std::shared_ptr<ce::InjectFrameRingLeaseState> injectRingLeaseState;
    try {
        injectRingLeaseState = std::make_shared<ce::InjectFrameRingLeaseState>(&media_main_g_pSharedMem->frameRing);
    } catch (const std::exception& error) {
        LogError("[Inject Thread] Failed to allocate frame-ring ownership state: %s", error.what());
        media_main_g_InjectCaptureRunning = false;
        return;
    }

    // PACING INITIALIZATION
    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    // Target interval in ticks (e.g. 1/120s)
    const int64_t targetIntervalTicks =
        (config.video.fps > 0) ? (qpcFreq.QuadPart / config.video.fps) : (qpcFreq.QuadPart / 60);
    const uint32_t injectPublicationFps =
        ce::capture_policy::GetInjectCfrSourcePublicationFps(static_cast<uint32_t>(std::max(config.video.fps, 1)));
    const int64_t injectPublicationIntervalTicks =
        std::max<int64_t>(1, ce::capture_policy::GetInjectCfrSourcePublicationIntervalQpc(
                                 static_cast<uint32_t>(std::max(config.video.fps, 1)), qpcFreq.QuadPart));
    int64_t nextPushTime = 0;

    DWORD lastLog = GetTickCount();
    uint32_t pushedCount = 0;
    uint32_t droppedCount = 0;
    uint32_t pacingDroppedCount = 0;
    uint32_t lastDuplicateCount = 0;
    uint32_t lastLateCount = 0;
    uint32_t lastTrimmedCount = media_main_g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
    uint32_t lastCadenceDroppedCount = media_main_g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
    uint32_t lastDeferredCount = media_main_g_InjectDeferredFrames.load(std::memory_order_relaxed);
    bool earlyTexturesCreated = false;
    bool sharedTexturesCreated = false;
    uint64_t publicationToIngestAccumUs = 0;
    uint32_t publicationToIngestSamples = 0;
    uint32_t publicationToIngestMaxUs = 0;

    while (!media_main_g_InjectCaptureShutdown && media_main_g_Recording) {
        applySchedulingForCurrentAdapter();
        // Create encoder textures as soon as resolution is available (before frames arrive)
        // This is critical for DXVK where the Vulkan layer waits for encoder KMT textures
        // NOTE: non-static so it resets per thread lifetime (new recording = new thread)
        if (!earlyTexturesCreated && media_main_g_pSharedMem->GetWidth() > 0 && media_main_g_pSharedMem->GetHeight() > 0) {
            if (!media_main_g_pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
                if (MediaEngine_CreateSharedCaptureTextures(media_main_g_pSharedMem->GetWidth(), media_main_g_pSharedMem->GetHeight(),
                                                            media_main_g_pSharedMem->GetFormat(), media_main_g_pSharedMem)) {
                    LogInfo("[Inject Thread] Created encoder KMT textures early: %dx%d", media_main_g_pSharedMem->GetWidth(),
                            media_main_g_pSharedMem->GetHeight());
                    earlyTexturesCreated = true;
                }
            } else {
                earlyTexturesCreated = true;
            }
        }

        // 1. Check for new frames
        uint32_t writeIndex = media_main_g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);

        // Overflow Protection
        const uint32_t unreadRingEntries = writeIndex - localReadIndex;
        if (unreadRingEntries > static_cast<uint32_t>(FRAME_RING_SIZE)) {
            uint32_t dropped = unreadRingEntries - 1;
            // Only log huge jumps to avoid spam
            if (dropped > 10) {
                LogInfo("[Inject Thread] Lag detected! Dropping %u frames to catch up", dropped);
            }
            const uint32_t catchupReadIndex = writeIndex - 1;
            while (localReadIndex != catchupReadIndex) {
                injectRingLeaseState->Complete(localReadIndex);
                advanceIngestIndex();
            }
            droppedCount += dropped;
            media_main_g_pSharedMem->runtimeState.hostDroppedFrames.fetch_add(dropped, std::memory_order_relaxed);
            // Reset pacing on overflow/lag
            nextPushTime = 0;
        }

        if (writeIndex != localReadIndex) {
            uint32_t index = localReadIndex % FRAME_RING_SIZE;
            FrameSlot& slot = media_main_g_pSharedMem->frameRing.slots[index];

            if (slot.valid.load(std::memory_order_acquire)) {
                // CRITICAL FIX: Ensure all slot fields are visible after valid flag
                // The acquire on valid provides synchronization with the producer's release,
                // but we add an explicit fence to prevent compiler reordering of reads.
                std::atomic_thread_fence(std::memory_order_acquire);

                // PACING CHECK:
                // The media thread owns CFR source selection.  In normal shallow-queue
                // operation, keep a high-rate publication stream so the selector has
                // before/after candidates for every output grid tick.  Only fall back to
                // source-side target pacing under real queue pressure.
                bool shouldProcess = false;

                const uint32_t queueDepth = static_cast<uint32_t>(media_main_g_FrameQueue.Size());
                const uint32_t queuePressureThreshold =
                    std::max<uint32_t>(24u, static_cast<uint32_t>(media_main_g_FrameQueue.Capacity() * 3 / 4));
                const bool useSourceSidePacing = queueDepth >= queuePressureThreshold;
                const int64_t pacingIntervalTicks =
                    useSourceSidePacing ? targetIntervalTicks : injectPublicationIntervalTicks;

                if (nextPushTime == 0) {
                    // First frame or resync
                    nextPushTime = slot.timestamp;
                    shouldProcess = true;
                } else {
                    const int64_t jitterWindow = useSourceSidePacing ? (pacingIntervalTicks * 8) / 10
                                                                     : std::max<int64_t>(1, pacingIntervalTicks / 8);

                    if (slot.timestamp >= nextPushTime - jitterWindow) {
                        shouldProcess = true;

                        // Advance target time by actual interval, not to current timestamp
                        // This maintains steady output cadence even with jittery input
                        nextPushTime += pacingIntervalTicks;

                        // Resync if game time jumped way ahead (e.g. pause/lag spike > 5
                        // frames) Increased from 3 to 5 frames to avoid unnecessary resyncs
                        if (slot.timestamp > nextPushTime + (pacingIntervalTicks * 5)) {
                            nextPushTime = slot.timestamp + pacingIntervalTicks;
                        }
                    } else {
                        // Frame is too early - only drop if we're not behind on processing
                        // Check if we have a backlog of frames waiting
                        uint32_t pendingFrames = (writeIndex > localReadIndex) ? (writeIndex - localReadIndex) : 0;

                        if (useSourceSidePacing && pendingFrames > 2) {
                            // We have a backlog, process this frame anyway to catch up
                            shouldProcess = true;
                            nextPushTime = slot.timestamp + pacingIntervalTicks;
                        } else {
                            // Frame is genuinely too early and no backlog - safe to drop
                            shouldProcess = false;
                            pacingDroppedCount++;
                            media_main_g_pSharedMem->runtimeState.injectPacingDrops.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }

                if (shouldProcess) {
                    // Validate shared memory frame data before using it
                    uint32_t fw = media_main_g_pSharedMem->GetWidth();
                    uint32_t fh = media_main_g_pSharedMem->GetHeight();
                    int32_t texIdx = slot.textureIndex;

                    bool dropFrame = false;
                    if (fw == 0 || fh == 0 || fw > 15360 || fh > 8640 || texIdx < 0 || texIdx > 200) {
                        if (localReadIndex % 100 == 0) {
                            LogWarn("[Inject Thread] Invalid frame data: %ux%u texIdx=%d, dropping", fw, fh, texIdx);
                        }
                        media_main_g_pSharedMem->runtimeState.invalidFrameMetadata.fetch_add(1, std::memory_order_relaxed);
                        dropFrame = true;
                    }

                    QueuedFrame qf;
                    qf.isInjectMode = true;
                    qf.ringIndex = localReadIndex;
                    qf.frameIndex = slot.frameIndex;
                    qf.textureIndex = texIdx;
                    qf.displayTimingSequence = slot.displayTimingSequence;
                    qf.displayTimingGeneration = slot.displayTimingGeneration;
                    qf.captureFlags = slot.captureFlags;
                    qf.injectRingLease = injectRingLeaseState->Acquire(localReadIndex);
                    qf.timestamp = slot.timestamp;
                    qf.rawTimestamp = slot.timestamp;
                    LARGE_INTEGER enqueueQpc;
                    QueryPerformanceCounter(&enqueueQpc);
                    qf.enqueueQpc = enqueueQpc.QuadPart;
                    if (slot.timestamp > 0 && enqueueQpc.QuadPart >= slot.timestamp && qpcFreq.QuadPart > 0) {
                        const uint64_t ingestDelayUs = static_cast<uint64_t>(enqueueQpc.QuadPart - slot.timestamp) *
                                                       1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                        publicationToIngestAccumUs += ingestDelayUs;
                        ++publicationToIngestSamples;
                        publicationToIngestMaxUs =
                            std::max(publicationToIngestMaxUs, SaturatingToUint32(ingestDelayUs));
                    }

                    static int64_t s_lastInjectTimestamp = 0;
                    if (s_lastInjectTimestamp > 0) {
                        if (slot.timestamp < s_lastInjectTimestamp) {
                            media_main_g_pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                        } else if (slot.timestamp == s_lastInjectTimestamp) {
                            media_main_g_pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    s_lastInjectTimestamp = slot.timestamp;

                    if (texIdx >= 100) {
                        qf.isShmem = true;
                        qf.shmemSlot = texIdx - 100;
                        qf.sharedHandle = nullptr;
                        qf.fenceHandle = nullptr;
                        qf.fenceValue = 0;
                    } else {
                        qf.isShmem = false;
                        qf.shmemSlot = 0;
                        if (IsValidTextureIndex(texIdx)) {
                            qf.sharedHandle = (HANDLE)media_main_g_pSharedMem->GetSharedHandle(texIdx);
                            // Metered diagnostic: slot handles are allocated once
                            // per shared-memory setup and rarely change, yet this
                            // line used to fire on every ingested frame (~16
                            // lines/frame in trace sessions). Log on first
                            // observation per slot and on actual handle changes
                            // only; that preserves the full handle-transition
                            // history without the per-frame noise.
                            static uint64_t s_lastLoggedSharedHandle[SHARED_TEXTURE_SLOT_COUNT] = {};
                            static bool s_handleEverLogged[SHARED_TEXTURE_SLOT_COUNT] = {};
                            const uint64_t handleValue = reinterpret_cast<uint64_t>(qf.sharedHandle);
                            if (!s_handleEverLogged[texIdx] || s_lastLoggedSharedHandle[texIdx] != handleValue) {
                                LogDebug("[Inject Thread] Read handle for texIdx=%d: %p", texIdx, qf.sharedHandle);
                                s_handleEverLogged[texIdx] = true;
                                s_lastLoggedSharedHandle[texIdx] = handleValue;
                            }
                        } else {
                            qf.sharedHandle = (HANDLE)media_main_g_pSharedMem->GetSharedHandle(0);
                            LogDebug("[Inject Thread] Invalid texIdx=%d, using handle 0: %p", texIdx, qf.sharedHandle);
                        }
                        const bool useEncoderTextureFence =
                            media_main_g_pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
                        qf.fenceHandle = useEncoderTextureFence ? (HANDLE)media_main_g_pSharedMem->encoderTextures.GetFenceHandle()
                                                                : (HANDLE)media_main_g_pSharedMem->GetFenceShareHandle();
                        qf.fenceValue = slot.fenceValue;
                    }

                    qf.sourcePid = slot.sourcePid;
                    qf.width = media_main_g_pSharedMem->GetWidth();
                    qf.height = media_main_g_pSharedMem->GetHeight();
                    qf.format = media_main_g_pSharedMem->GetFormat();
                    qf.luidLow = media_main_g_pSharedMem->GetLuidLowPart();
                    qf.luidHigh = media_main_g_pSharedMem->GetLuidHighPart();
                    qf.isHDR = media_main_g_pSharedMem->GetIsHDR();

                    // Inject textures are swap-chain-local, while Windows cursor
                    // coordinates are desktop-global. Resolve the game client
                    // area once per PID and map through its current physical
                    // bounds so windowed, borderless, DPI, and render-scale
                    // configurations all place the cursor correctly.
                    static DWORD s_cursorWindowPid = 0;
                    static HWND s_cursorWindow = NULL;
                    if (qf.sourcePid != s_cursorWindowPid || !s_cursorWindow || !IsWindow(s_cursorWindow) ||
                        !WindowBelongsToProcess(s_cursorWindow, qf.sourcePid)) {
                        s_cursorWindowPid = qf.sourcePid;
                        s_cursorWindow = qf.sourcePid != 0 ? GetMainWindowForProcess(qf.sourcePid) : NULL;
                    }
                    RECT captureBounds = {0, 0, static_cast<LONG>(qf.width), static_cast<LONG>(qf.height)};
                    RECT clientBounds = {};
                    if (s_cursorWindow && GetWindowClientRectInScreen(s_cursorWindow, clientBounds) &&
                        clientBounds.right > clientBounds.left && clientBounds.bottom > clientBounds.top) {
                        captureBounds = clientBounds;
                    }
                    qf.captureLeft = captureBounds.left;
                    qf.captureTop = captureBounds.top;
                    qf.cursorState =
                        CaptureCursorSnapshot(qf.timestamp, captureBounds.left, captureBounds.top,
                                              static_cast<uint32_t>(captureBounds.right - captureBounds.left),
                                              static_cast<uint32_t>(captureBounds.bottom - captureBounds.top), false);
                    media_main_g_InjectCursorTimeline.Publish(qf.cursorState);

                    // Per-recording state (reset on thread creation)
                    if (!sharedTexturesCreated && media_main_g_pSharedMem->GetWidth() > 0 && media_main_g_pSharedMem->GetHeight() > 0) {
                        if (!media_main_g_pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                            if (MediaEngine_CreateSharedCaptureTextures(media_main_g_pSharedMem->GetWidth(),
                                                                        media_main_g_pSharedMem->GetHeight(),
                                                                        media_main_g_pSharedMem->GetFormat(), media_main_g_pSharedMem)) {
                                sharedTexturesCreated = true;
                            }
                        } else {
                            sharedTexturesCreated = true;
                        }
                    }

                    // Validate handles look reasonable (not 0, not -1, not obviously stale)
                    bool validHandles = true;
                    if (!qf.isShmem) {
                        uint64_t handleVal = (uint64_t)qf.sharedHandle;
                        // Reject obviously invalid handles
                        if (handleVal == 0 || handleVal == 0xFFFFFFFFFFFFFFFF || handleVal == 0xCCCCCCCCCCCCCCCC ||
                            handleVal == 0xDDDDDDDDDDDDDDDD) {
                            LogInfo("[Inject Thread] Invalid handle detected (0x%p), skipping frame", qf.sharedHandle);
                            media_main_g_pSharedMem->runtimeState.invalidSharedHandles.fetch_add(1, std::memory_order_relaxed);
                            validHandles = false;
                        }
                    }

                    if (!dropFrame && validHandles) {
                        if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire)) {
                            droppedCount++;
                            dropFrame = true;
                            qf.injectRingLease.Reset();
                        } else {
                            // Push cannot fail and always takes ownership; qf is moved-from
                            // below, so no post-push recovery path may touch it.
                            const InjectFrameLineage lineage = MakeInjectFrameLineage(qf);
                            media_main_g_FrameQueue.Push(std::move(qf));
                            static uint64_t s_lastQueuedLineageLogTick = 0;
                            const uint64_t nowTick = GetTickCount64();
                            if (nowTick - s_lastQueuedLineageLogTick >= 1000) {
                                const uint32_t ringWrite =
                                    media_main_g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
                                const uint32_t ringAckRead =
                                    media_main_g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
                                const uint32_t nextIngest = localReadIndex + 1;
                                LogInfo(
                                    "[Inject Thread] Queue frame=%u ring=%u tex=%d fence=%llu ts=%lld qDepth=%u "
                                    "ringIngestNext=%u ringAckRead=%u ringWrite=%u ownedDepth=%u",
                                    lineage.frameIndex, lineage.ringIndex, lineage.textureIndex,
                                    static_cast<unsigned long long>(lineage.fenceValue),
                                    static_cast<long long>(lineage.timestamp), queueDepth, nextIngest, ringAckRead,
                                    ringWrite, static_cast<uint32_t>(ringWrite - ringAckRead));
                                s_lastQueuedLineageLogTick = nowTick;
                            }
                            if (!media_main_g_InjectDeliveredFirstFrame.exchange(true, std::memory_order_acq_rel)) {
                                LogInfo("[Inject Thread] First actual inject frame queued");
                            }
                            pushedCount++;
                            media_main_g_pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        droppedCount++;
                        dropFrame = true;
                        qf.injectRingLease.Reset();
                    }

                    if (dropFrame) {
                        advanceIngestIndex();
                        continue;
                    }
                } else {
                    // Pacing drop: release the ring slot immediately so the producer
                    // does not stall behind frames that will never be encoded.
                    injectRingLeaseState->Complete(localReadIndex);
                    advanceIngestIndex();
                    continue;
                }

                advanceIngestIndex();
            } else {
                injectRingLeaseState->Complete(localReadIndex);
                advanceIngestIndex();
            }
        } else {
            if (media_main_g_InjectFrameReadyEvent && media_main_g_InjectCaptureShutdownEvent) {
                HANDLE waitHandles[] = {media_main_g_InjectCaptureShutdownEvent, media_main_g_InjectFrameReadyEvent};
                const DWORD waitResult = WaitForMultipleObjects(_countof(waitHandles), waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) {
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    LogWarn("[Inject Thread] Frame-event wait failed (err=%lu); using bounded shutdown wait",
                            GetLastError());
                    WaitForSingleObject(media_main_g_InjectCaptureShutdownEvent, 1);
                }
            } else if (media_main_g_InjectCaptureShutdownEvent) {
                WaitForSingleObject(media_main_g_InjectCaptureShutdownEvent, 1);
            } else {
                SwitchToThread();
            }
        }

        DWORD now = GetTickCount();
        if (now - lastLog >= 1000) {
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t trimDelta = 0;
            uint32_t cadenceDropDelta = 0;
            uint32_t deferredDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(media_main_g_FrameQueue.Size());
            uint32_t ringReadIndex = 0;
            uint32_t ringWriteIndex = 0;
            if (media_main_g_pSharedMem) {
                ringReadIndex = media_main_g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
                ringWriteIndex = media_main_g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
                uint32_t currentDup = media_main_g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = media_main_g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                uint32_t currentTrimmed = media_main_g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = media_main_g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
                uint32_t currentDeferred = media_main_g_InjectDeferredFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                deferredDelta = currentDeferred - lastDeferredCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
                lastDeferredCount = currentDeferred;
                overloadFlags = media_main_g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = media_main_g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = media_main_g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.injectTrimmedFrames.store(currentTrimmed, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.injectCadenceDrops.store(currentCadenceDropped, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.deferredFrames.store(currentDeferred, std::memory_order_relaxed);
            } else {
                uint32_t currentTrimmed = media_main_g_InjectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = media_main_g_InjectCadenceDroppedFrames.load(std::memory_order_relaxed);
                uint32_t currentDeferred = media_main_g_InjectDeferredFrames.load(std::memory_order_relaxed);
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                deferredDelta = currentDeferred - lastDeferredCount;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
                lastDeferredCount = currentDeferred;
            }

            uint32_t inputFrames = pushedCount + droppedCount + pacingDroppedCount;
            media_main_g_pSharedMem->runtimeState.sourceFramesReceived.fetch_add(inputFrames, std::memory_order_relaxed);
            LogInfo(
                "[Inject Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | PubFps: %u | HostQ: %u | EncQ: "
                "%u | RingR: %u | RingW: %u | RingDepth: %u | Dup: %u | Late: %u | Trim: %u | SelDrop: %u | Def: %u "
                "| Encode: %lldus | Fence: %lldus | Mux: %uKB | Overload: 0x%X",
                inputFrames, pushedCount, droppedCount, pacingDroppedCount, injectPublicationFps,
                static_cast<uint32_t>(media_main_g_FrameQueue.Size()), encoderQueueDepth, ringReadIndex, ringWriteIndex,
                static_cast<uint32_t>(ringWriteIndex - ringReadIndex), dupDelta, lateDelta, trimDelta, cadenceDropDelta,
                deferredDelta, MediaEngine_GetLastFrameEncodeTimeUs(), MediaEngine_GetLastFrameFenceWaitUs(),
                (muxQueueBytes + 1023u) / 1024u, overloadFlags);
            auto& contention = media_main_g_pSharedMem->runtimeState;
            const uint32_t ingestAvgUs =
                publicationToIngestSamples > 0
                    ? SaturatingToUint32(publicationToIngestAccumUs / publicationToIngestSamples)
                    : 0;
            contention.injectPublicationToIngestAvgUs.store(ingestAvgUs, std::memory_order_relaxed);
            contention.injectPublicationToIngestMaxUs.store(publicationToIngestMaxUs, std::memory_order_relaxed);
            LogInfo(
                "[Inject Contention] CaptureLock=%u CpuLease=%u GpuBusy=%u RingFull=%u EventSignals=%u "
                "PubToIngest=%u/%uus",
                contention.injectProducerCaptureLockDrops.load(std::memory_order_relaxed),
                contention.injectProducerCpuLeaseBusyDrops.load(std::memory_order_relaxed),
                contention.injectProducerGpuBusyDrops.load(std::memory_order_relaxed),
                contention.injectProducerMetadataFullDrops.load(std::memory_order_relaxed),
                contention.injectFrameReadySignals.load(std::memory_order_relaxed), ingestAvgUs,
                publicationToIngestMaxUs);
            publicationToIngestAccumUs = 0;
            publicationToIngestSamples = 0;
            publicationToIngestMaxUs = 0;
            pushedCount = 0;
            droppedCount = 0;
            pacingDroppedCount = 0;
            lastLog = now;
        }
    }

    media_main_g_InjectCaptureRunning = false;
    LogInfo("[Inject Thread] Stopped");
}
