#include "media_main_internal.h"

#include <tlhelp32.h>

#include "common/capture/inject_frame_ring_ingest.h"
#include "common/capture/inject_frame_source_policy.h"
#include "common/ipc/inject_transport_snapshot.h"

namespace {

// Whether `rendererPid` is a live direct child of `clientPid`. This is the same
// topology the Vulkan layer proved before it published its split-renderer claim
// (hook/vulkan_layer/layer_participation.h); re-checking it here keeps a claim
// left behind by a dead renderer, or a recycled PID, from naming a process media
// would then open with PROCESS_DUP_HANDLE.
ce::inject_frame_source::ParentCheck CheckDirectChildProcess(uint32_t rendererPid, uint32_t clientPid) {
    using ce::inject_frame_source::ParentCheck;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return ParentCheck::kUnavailable;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    ParentCheck result = ParentCheck::kUnavailable;
    if (Process32FirstW(snapshot, &entry)) {
        result = ParentCheck::kNotDirectChild;
        do {
            if (entry.th32ProcessID == rendererPid) {
                if (entry.th32ParentProcessID == clientPid)
                    result = ParentCheck::kDirectChild;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

const char* ParentCheckName(ce::inject_frame_source::ParentCheck check) {
    switch (check) {
        case ce::inject_frame_source::ParentCheck::kDirectChild:
            return "direct-child";
        case ce::inject_frame_source::ParentCheck::kNotDirectChild:
            return "not-direct-child";
        case ce::inject_frame_source::ParentCheck::kUnavailable:
            return "snapshot-unavailable";
    }
    return "unknown";
}

}  // namespace

void InjectCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[Inject Thread] Started (event-driven ingest with adaptive source-side pacing)");
    MediaState().injectCaptureRunning = true;
    DisableCurrentThreadPowerThrottling("Inject Thread");
    ScopedMmcssTask injectMmcssTask(L"Capture", AVRT_PRIORITY_HIGH, "Inject Thread");

    if (!MediaState().pSharedMem) {
        LogError("[Inject Thread] Shared memory not available! Aborting.");
        MediaState().injectCaptureRunning = false;
        return;
    }

    LUID lastSchedulingLuid{};
    bool haveSchedulingLuid = false;
    const auto applySchedulingForCurrentAdapter = [&]() {
        LUID current{};
        current.LowPart = MediaState().pSharedMem->GetLuidLowPart();
        current.HighPart = static_cast<LONG>(MediaState().pSharedMem->GetLuidHighPart());
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

    // The ring this thread consumes, its ingest cursor and its leases stay bound to one ring for the thread's life
    // (common/capture/inject_frame_ring_ingest.h). Ingestion resumes independently of lease acknowledgements: a
    // prior recording can still own slots whose encoder GPU reads timed out.
    FrameRingBuffer& ring = MediaState().pSharedMem->frameRing;
    std::shared_ptr<ce::InjectFrameRingLeaseState> injectRingLeaseState;
    try {
        injectRingLeaseState = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
    } catch (const std::exception& error) {
        LogError("[Inject Thread] Failed to allocate frame-ring ownership state: %s", error.what());
        MediaState().injectCaptureRunning = false;
        return;
    }
    ce::InjectFrameRingIngestCursor ingest(ring, injectRingLeaseState);
    uint32_t ringCursorFaults = 0;

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
    uint32_t lastTrimmedCount = MediaState().injectBufferedTrimmedFrames.load(std::memory_order_relaxed);
    uint32_t lastCadenceDroppedCount = MediaState().injectCadenceDroppedFrames.load(std::memory_order_relaxed);
    uint32_t lastDeferredCount = MediaState().injectDeferredFrames.load(std::memory_order_relaxed);
    bool earlyTexturesCreated = false;
    bool sharedTexturesCreated = false;
    ce::inject_frame_source::SplitRendererVerificationCache splitRendererVerification;
    uint64_t publicationToIngestAccumUs = 0;
    uint32_t publicationToIngestSamples = 0;
    uint32_t publicationToIngestMaxUs = 0;
    // Frames stamped with the Present they reach after the hook's FPS limiter wait arrive before
    // their own timestamp; they have no publication-to-ingest delay to measure.
    uint32_t stampAheadOfIngestFrames = 0;
    uint32_t stampAheadOfIngestMaxUs = 0;

    while (!MediaState().injectCaptureShutdown && MediaState().recording) {
        applySchedulingForCurrentAdapter();
        // Slots whose encoder GPU read finished go back to the producer first:
        // a producer with no free slot publishes nothing that would wake us.
        ReleaseCompletedInjectSourceReads();
        // Create encoder textures as soon as resolution is available (before frames arrive)
        // This is critical for DXVK where the Vulkan layer waits for encoder KMT textures
        // NOTE: non-static so it resets per thread lifetime (new recording = new thread)
        if (!earlyTexturesCreated && MediaState().pSharedMem->GetWidth() > 0 && MediaState().pSharedMem->GetHeight() > 0) {
            if (!MediaState().pSharedMem->encoderTextures.kmtReady.load(std::memory_order_acquire)) {
                if (ce::mediaengine_api::MediaEngine_CreateSharedCaptureTextures(
                        MediaState().engine, MediaState().pSharedMem->GetWidth(),
                        MediaState().pSharedMem->GetHeight(), MediaState().pSharedMem->GetFormat(),
                        MediaState().pSharedMem)) {
                    LogInfo("[Inject Thread] Created encoder KMT textures early: %dx%d", MediaState().pSharedMem->GetWidth(),
                            MediaState().pSharedMem->GetHeight());
                    earlyTexturesCreated = true;
                }
            } else {
                earlyTexturesCreated = true;
            }
        }

        // 1. Check for new frames
        uint32_t writeIndex = ring.writeIndex.load(std::memory_order_acquire);

        const ce::InjectFrameRingResync resync = ingest.ResyncIfOverrun(writeIndex);
        if (resync.faulted) {
            ++ringCursorFaults;
            if (ringCursorFaults <= 8 || (ringCursorFaults % 1000) == 0) {
                LogError(
                    "[Inject Thread] Frame ring cursor fault #%u: %u unread entries exceed the %d-slot ring "
                    "(ingest=%u read=%u write=%u); resuming at the newest publication, %u in-window slot(s) skipped",
                    ringCursorFaults, static_cast<uint32_t>(writeIndex - resync.cursor), FRAME_RING_SIZE,
                    resync.cursor, ring.readIndex.load(std::memory_order_acquire), writeIndex, resync.skipped);
            }
            droppedCount += resync.skipped;
            MediaState().pSharedMem->runtimeState.hostDroppedFrames.fetch_add(resync.skipped, std::memory_order_relaxed);
            // Reset pacing on overflow/lag
            nextPushTime = 0;
        }

        if (writeIndex != ingest.Next()) {
            const uint32_t ringIndex = ingest.Next();
            const FrameSlot& slot = ingest.NextSlot();

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

                const uint32_t queueDepth = static_cast<uint32_t>(MediaState().frameQueue.Size());
                const uint32_t queuePressureThreshold =
                    std::max<uint32_t>(24u, static_cast<uint32_t>(MediaState().frameQueue.Capacity() * 3 / 4));
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
                        uint32_t pendingFrames = (writeIndex > ringIndex) ? (writeIndex - ringIndex) : 0;

                        if (useSourceSidePacing && pendingFrames > 2) {
                            // We have a backlog, process this frame anyway to catch up
                            shouldProcess = true;
                            nextPushTime = slot.timestamp + pacingIntervalTicks;
                        } else {
                            // Frame is genuinely too early and no backlog - safe to drop
                            shouldProcess = false;
                            pacingDroppedCount++;
                            MediaState().pSharedMem->runtimeState.injectPacingDrops.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }

                if (shouldProcess) {
                    // Validate shared memory frame data before using it
                    uint32_t fw = MediaState().pSharedMem->GetWidth();
                    uint32_t fh = MediaState().pSharedMem->GetHeight();
                    int32_t texIdx = slot.textureIndex;

                    bool dropFrame = false;
                    if (fw == 0 || fh == 0 || fw > 15360 || fh > 8640 || texIdx < 0 || texIdx > 200) {
                        if (ringIndex % 100 == 0) {
                            LogWarn("[Inject Thread] Invalid frame data: %ux%u texIdx=%d, dropping", fw, fh, texIdx);
                        }
                        MediaState().pSharedMem->runtimeState.invalidFrameMetadata.fetch_add(1, std::memory_order_relaxed);
                        dropFrame = true;
                    }

                    QueuedFrame qf;
                    qf.isInjectMode = true;
                    qf.ringIndex = ringIndex;
                    qf.frameIndex = slot.frameIndex;
                    qf.textureIndex = texIdx;
                    qf.displayTimingSequence = slot.displayTimingSequence;
                    qf.displayTimingGeneration = slot.displayTimingGeneration;
                    qf.captureFlags = slot.captureFlags;
                    // Ingestion moves past the slot here; the lease keeps it from reuse while it is read below and
                    // until the frame's owner is done with its texture.
                    qf.injectRingLease = ingest.Take();
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
                    } else if (slot.timestamp > enqueueQpc.QuadPart && qpcFreq.QuadPart > 0) {
                        const uint64_t aheadUs = static_cast<uint64_t>(slot.timestamp - enqueueQpc.QuadPart) *
                                                 1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                        ++stampAheadOfIngestFrames;
                        stampAheadOfIngestMaxUs = std::max(stampAheadOfIngestMaxUs, SaturatingToUint32(aheadUs));
                    }

                    if (MediaState().lastInjectTimestamp > 0) {
                        if (slot.timestamp < MediaState().lastInjectTimestamp) {
                            MediaState().pSharedMem->runtimeState.sourceTimestampRegressions.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                        } else if (slot.timestamp == MediaState().lastInjectTimestamp) {
                            MediaState().pSharedMem->runtimeState.sourceTimestampStalls.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    MediaState().lastInjectTimestamp = slot.timestamp;

                    qf.transportGeneration = slot.transportGeneration;
                    if (texIdx >= 100) {
                        qf.isShmem = true;
                        qf.shmemSlot = texIdx - 100;
                        qf.sharedHandle = nullptr;
                        qf.fenceHandle = nullptr;
                        qf.fenceValue = 0;
                    } else {
                        qf.isShmem = false;
                        qf.shmemSlot = 0;
                        const bool useEncoderTextureFence =
                            MediaState().pSharedMem->useEncoderTextures.load(std::memory_order_acquire);
                        const ce::InjectTransportSnapshot transport = ce::ReadInjectTransportSnapshot(
                            *MediaState().pSharedMem, IsValidTextureIndex(texIdx) ? texIdx : 0,
                            useEncoderTextureFence, slot.transportGeneration);
                        if (!transport.consistent) {
                            // The producer re-created its shared transport after publishing
                            // this frame; the handles now in shared memory belong to the new
                            // generation and may even reuse this frame's old handle values.
                            ++MediaState().staleTransportDrops;
                            if (MediaState().staleTransportDrops <= 16 || (MediaState().staleTransportDrops % 1000) == 0) {
                                LogInfo(
                                    "[Inject Thread] Dropping frame=%u ring=%u from transport generation %u "
                                    "(current=%llu): the producer re-created its shared textures/fence (drops=%llu)",
                                    slot.frameIndex, ringIndex, slot.transportGeneration,
                                    static_cast<unsigned long long>(transport.generation),
                                    static_cast<unsigned long long>(MediaState().staleTransportDrops));
                            }
                            dropFrame = true;
                        }
                        if (IsValidTextureIndex(texIdx)) {
                            qf.sharedHandle = (HANDLE)transport.sharedHandle;
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
                            qf.sharedHandle = (HANDLE)transport.sharedHandle;
                            LogDebug("[Inject Thread] Invalid texIdx=%d, using handle 0: %p", texIdx, qf.sharedHandle);
                        }
                        qf.fenceHandle = (HANDLE)transport.fenceHandle;
                        qf.fenceValue = slot.fenceValue;
                    }

                    // Pin the frame's source PID to the process that owns this capture session.
                    //
                    // qf.sourcePid decides which process the encoder opens with
                    // PROCESS_DUP_HANDLE to duplicate the frame's texture and fence handles
                    // (video_encoder_encode_input.cpp). It arrives from the inject frame ring in
                    // shared memory, which is written by the hook inside the game - so it is
                    // session data, not something the encoder should take on trust. The session
                    // owner publishes its own PID once (hook/runtime/main_hookthread.cpp SetSourcePid),
                    // and a slot that disagrees is either stale from a previous capture epoch or
                    // was not written by the process CE injected into. Either way the handles in
                    // it cannot be resolved against the current source, so drop the frame rather
                    // than open an unrelated process.
                    //
                    // The one other legitimate writer is a split renderer: the Vulkan layer in a
                    // direct child that presents for the session source (Portal RTX's
                    // NvRemixBridge.exe for hl2.exe). Its slots carry its own PID because the
                    // handles live in its handle table. Admit it only when its ownership claim
                    // names the session source as client and it is still that source's direct
                    // child (common/capture/inject_frame_source_policy.h). Rejecting it here left Remix
                    // recordings stuck before the first live frame (session 20260929_031827).
                    const uint32_t sessionSourcePid = MediaState().pSharedMem->GetSourcePid();
                    const uint64_t vulkanLayerClaim =
                        MediaState().pSharedMem->runtimeState.vulkanLayerClaim.load(std::memory_order_acquire);
                    const uint32_t claimRendererPid = ce::vulkan_layer_claim::RendererPid(vulkanLayerClaim);
                    const uint32_t claimClientPid = ce::vulkan_layer_claim::ClientPid(vulkanLayerClaim);
                    const ce::inject_frame_source::Admission sourceAdmission = ce::inject_frame_source::Classify(
                        slot.sourcePid, sessionSourcePid, claimRendererPid, claimClientPid);
                    bool sourceAdmitted = sourceAdmission == ce::inject_frame_source::Admission::kSessionSource;
                    if (sourceAdmission == ce::inject_frame_source::Admission::kSplitRendererCandidate) {
                        const auto verdict = splitRendererVerification.Verify(slot.sourcePid, sessionSourcePid,
                                                                              CheckDirectChildProcess);
                        sourceAdmitted = verdict.admitted;
                        if (verdict.ranCheck) {
                            if (verdict.admitted) {
                                LogInfo(
                                    "[Inject Thread] Admitting split-renderer frames: slot source PID %lu is the "
                                    "claimed Vulkan renderer for session source PID %lu (%s)",
                                    static_cast<unsigned long>(slot.sourcePid),
                                    static_cast<unsigned long>(sessionSourcePid), ParentCheckName(verdict.check));
                            } else {
                                LogWarn(
                                    "[Inject Thread] Refusing split-renderer frames: slot source PID %lu claims "
                                    "session source PID %lu as client but the process check says %s",
                                    static_cast<unsigned long>(slot.sourcePid),
                                    static_cast<unsigned long>(sessionSourcePid), ParentCheckName(verdict.check));
                            }
                        }
                    }
                    if (!sourceAdmitted) {
                        const uint32_t seen = MediaState().mismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (seen <= 5 || (seen % 500) == 0) {
                            LogWarn(
                                "[Inject Thread] Dropping frame whose slot source PID %lu does not match the "
                                "session source PID %lu (admission=%s vulkanClaim renderer=%lu client=%lu "
                                "occurrence %lu)",
                                static_cast<unsigned long>(slot.sourcePid),
                                static_cast<unsigned long>(sessionSourcePid),
                                ce::inject_frame_source::AdmissionName(sourceAdmission),
                                static_cast<unsigned long>(claimRendererPid),
                                static_cast<unsigned long>(claimClientPid),
                                static_cast<unsigned long>(seen));
                        }
                        // Use the existing drop path rather than `continue`: the only loop
                        // enclosing this point is the thread's outer while, so continuing here
                        // would skip the rest of the iteration, not just this frame.
                        dropFrame = true;
                    }
                    qf.sourcePid = (slot.sourcePid != 0) ? slot.sourcePid : sessionSourcePid;
                    qf.width = MediaState().pSharedMem->GetWidth();
                    qf.height = MediaState().pSharedMem->GetHeight();
                    qf.format = MediaState().pSharedMem->GetFormat();
                    qf.luidLow = MediaState().pSharedMem->GetLuidLowPart();
                    qf.luidHigh = MediaState().pSharedMem->GetLuidHighPart();
                    qf.isHDR = MediaState().pSharedMem->GetIsHDR();

                    // Inject textures are swap-chain-local, while Windows cursor
                    // coordinates are desktop-global. Resolve the game client
                    // area once per PID and map through its current physical
                    // bounds so windowed, borderless, DPI, and render-scale
                    // configurations all place the cursor correctly.
                    // The window belongs to the session source: a split renderer presents
                    // into its client's window and owns none itself.
                    const DWORD cursorWindowPid = sessionSourcePid != 0 ? sessionSourcePid : qf.sourcePid;
                    if (cursorWindowPid != MediaState().cursorWindowPid || !MediaState().cursorWindow || !IsWindow(MediaState().cursorWindow) ||
                        !WindowBelongsToProcess(MediaState().cursorWindow, cursorWindowPid)) {
                        MediaState().cursorWindowPid = cursorWindowPid;
                        MediaState().cursorWindow = cursorWindowPid != 0 ? GetMainWindowForProcess(cursorWindowPid) : NULL;
                    }
                    RECT captureBounds = {0, 0, static_cast<LONG>(qf.width), static_cast<LONG>(qf.height)};
                    RECT clientBounds = {};
                    if (MediaState().cursorWindow && GetWindowClientRectInScreen(MediaState().cursorWindow, clientBounds) &&
                        clientBounds.right > clientBounds.left && clientBounds.bottom > clientBounds.top) {
                        captureBounds = clientBounds;
                    }
                    qf.captureLeft = captureBounds.left;
                    qf.captureTop = captureBounds.top;
                    qf.cursorState =
                        CaptureCursorSnapshot(qf.timestamp, captureBounds.left, captureBounds.top,
                                              static_cast<uint32_t>(captureBounds.right - captureBounds.left),
                                              static_cast<uint32_t>(captureBounds.bottom - captureBounds.top), false);
                    MediaState().injectCursorTimeline.Publish(qf.cursorState);

                    // Per-recording state (reset on thread creation)
                    if (!sharedTexturesCreated && MediaState().pSharedMem->GetWidth() > 0 && MediaState().pSharedMem->GetHeight() > 0) {
                        if (!MediaState().pSharedMem->encoderTextures.ready.load(std::memory_order_acquire)) {
                            if (ce::mediaengine_api::MediaEngine_CreateSharedCaptureTextures(
                                    MediaState().engine, MediaState().pSharedMem->GetWidth(),
                                    MediaState().pSharedMem->GetHeight(), MediaState().pSharedMem->GetFormat(),
                                    MediaState().pSharedMem)) {
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
                            MediaState().pSharedMem->runtimeState.invalidSharedHandles.fetch_add(1, std::memory_order_relaxed);
                            validHandles = false;
                        }
                    }

                    if (!dropFrame && validHandles) {
                        if (MediaState().rejectInjectFrames.load(std::memory_order_acquire)) {
                            droppedCount++;
                            dropFrame = true;
                            qf.injectRingLease.Reset();
                        } else {
                            // Push cannot fail and always takes ownership; qf is moved-from
                            // below, so no post-push recovery path may touch it.
                            const InjectFrameLineage lineage = MakeInjectFrameLineage(qf);
                            MediaState().frameQueue.Push(std::move(qf));
                            static uint64_t s_lastQueuedLineageLogTick = 0;
                            const uint64_t nowTick = GetTickCount64();
                            if (nowTick - s_lastQueuedLineageLogTick >= 1000) {
                                const uint32_t ringWrite = ring.writeIndex.load(std::memory_order_acquire);
                                const uint32_t ringAckRead = ring.readIndex.load(std::memory_order_acquire);
                                const uint32_t nextIngest = ingest.Next();
                                LogInfo(
                                    "[Inject Thread] Queue frame=%u ring=%u tex=%d fence=%llu ts=%lld qDepth=%u "
                                    "ringIngestNext=%u ringAckRead=%u ringWrite=%u ownedDepth=%u",
                                    lineage.frameIndex, lineage.ringIndex, lineage.textureIndex,
                                    static_cast<unsigned long long>(lineage.fenceValue),
                                    static_cast<long long>(lineage.timestamp), queueDepth, nextIngest, ringAckRead,
                                    ringWrite, static_cast<uint32_t>(ringWrite - ringAckRead));
                                s_lastQueuedLineageLogTick = nowTick;
                            }
                            if (!MediaState().injectDeliveredFirstFrame.exchange(true, std::memory_order_acq_rel)) {
                                LogInfo("[Inject Thread] First actual inject frame queued");
                            }
                            pushedCount++;
                            MediaState().pSharedMem->runtimeState.framesQueued.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        droppedCount++;
                        dropFrame = true;
                        qf.injectRingLease.Reset();
                    }

                    if (dropFrame) {
                        continue;
                    }
                } else {
                    // Pacing drop: release the ring slot immediately so the producer
                    // does not stall behind frames that will never be encoded.
                    ingest.Skip();
                    continue;
                }
            } else {
                ingest.Skip();
            }
        } else {
            if (MediaState().injectFrameReadyEvent && MediaState().injectCaptureShutdownEvent) {
                HANDLE sourceReadEvent = MediaState().injectSourceReadReleases.CompletionEvent();
                HANDLE waitHandles[] = {MediaState().injectCaptureShutdownEvent, MediaState().injectFrameReadyEvent,
                                        sourceReadEvent};
                const DWORD waitCount = sourceReadEvent ? 3u : 2u;
                const DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) {
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    LogWarn("[Inject Thread] Frame-event wait failed (err=%lu); using bounded shutdown wait",
                            GetLastError());
                    WaitForSingleObject(MediaState().injectCaptureShutdownEvent, 1);
                }
            } else if (MediaState().injectCaptureShutdownEvent) {
                WaitForSingleObject(MediaState().injectCaptureShutdownEvent, 1);
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
            uint32_t encoderQueueDepth = static_cast<uint32_t>(MediaState().frameQueue.Size());
            uint32_t ringReadIndex = 0;
            uint32_t ringWriteIndex = 0;
            if (MediaState().pSharedMem) {
                ringReadIndex = ring.readIndex.load(std::memory_order_acquire);
                ringWriteIndex = ring.writeIndex.load(std::memory_order_acquire);
                uint32_t currentDup = MediaState().pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = MediaState().pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                uint32_t currentTrimmed = MediaState().injectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = MediaState().injectCadenceDroppedFrames.load(std::memory_order_relaxed);
                uint32_t currentDeferred = MediaState().injectDeferredFrames.load(std::memory_order_relaxed);
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
                overloadFlags = MediaState().pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = MediaState().pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = MediaState().pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.injectTrimmedFrames.store(currentTrimmed, std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.injectCadenceDrops.store(currentCadenceDropped, std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.deferredFrames.store(currentDeferred, std::memory_order_relaxed);
            } else {
                uint32_t currentTrimmed = MediaState().injectBufferedTrimmedFrames.load(std::memory_order_relaxed);
                uint32_t currentCadenceDropped = MediaState().injectCadenceDroppedFrames.load(std::memory_order_relaxed);
                uint32_t currentDeferred = MediaState().injectDeferredFrames.load(std::memory_order_relaxed);
                trimDelta = currentTrimmed - lastTrimmedCount;
                cadenceDropDelta = currentCadenceDropped - lastCadenceDroppedCount;
                deferredDelta = currentDeferred - lastDeferredCount;
                lastTrimmedCount = currentTrimmed;
                lastCadenceDroppedCount = currentCadenceDropped;
                lastDeferredCount = currentDeferred;
            }

            uint32_t inputFrames = pushedCount + droppedCount + pacingDroppedCount;
            MediaState().pSharedMem->runtimeState.sourceFramesReceived.fetch_add(inputFrames, std::memory_order_relaxed);
            LogInfo(
                "[Inject Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | PubFps: %u | HostQ: %u | EncQ: "
                "%u | RingR: %u | RingW: %u | RingDepth: %u | Dup: %u | Late: %u | Trim: %u | SelDrop: %u | Def: %u "
                "| Encode: %lldus | Fence: %lldus | Mux: %uKB | Overload: 0x%X",
                inputFrames, pushedCount, droppedCount, pacingDroppedCount, injectPublicationFps,
                static_cast<uint32_t>(MediaState().frameQueue.Size()), encoderQueueDepth, ringReadIndex, ringWriteIndex,
                static_cast<uint32_t>(ringWriteIndex - ringReadIndex), dupDelta, lateDelta, trimDelta, cadenceDropDelta,
                deferredDelta, ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs(MediaState().engine),
                ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs(MediaState().engine),
                (muxQueueBytes + 1023u) / 1024u, overloadFlags);
            auto& contention = MediaState().pSharedMem->runtimeState;
            const uint32_t ingestAvgUs =
                publicationToIngestSamples > 0
                    ? SaturatingToUint32(publicationToIngestAccumUs / publicationToIngestSamples)
                    : 0;
            contention.injectPublicationToIngestAvgUs.store(ingestAvgUs, std::memory_order_relaxed);
            contention.injectPublicationToIngestMaxUs.store(publicationToIngestMaxUs, std::memory_order_relaxed);
            LogInfo(
                "[Inject Contention] CaptureLock=%u CpuLease=%u GpuBusy=%u RingFull=%u EventSignals=%u "
                "PubToIngest=%u/%uus StampAhead=%u/%uus",
                contention.injectProducerCaptureLockDrops.load(std::memory_order_relaxed),
                contention.injectProducerCpuLeaseBusyDrops.load(std::memory_order_relaxed),
                contention.injectProducerGpuBusyDrops.load(std::memory_order_relaxed),
                contention.injectProducerMetadataFullDrops.load(std::memory_order_relaxed),
                contention.injectFrameReadySignals.load(std::memory_order_relaxed), ingestAvgUs,
                publicationToIngestMaxUs, stampAheadOfIngestFrames, stampAheadOfIngestMaxUs);
            publicationToIngestAccumUs = 0;
            publicationToIngestSamples = 0;
            publicationToIngestMaxUs = 0;
            stampAheadOfIngestFrames = 0;
            stampAheadOfIngestMaxUs = 0;
            LogInjectSourceReadWindow(qpcFreq.QuadPart);
            pushedCount = 0;
            droppedCount = 0;
            pacingDroppedCount = 0;
            lastLog = now;
        }
    }

    MediaState().injectCaptureRunning = false;
    LogInfo("[Inject Thread] Stopped");
}
