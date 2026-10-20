#pragma once

// The media role's state for one media session (DR-41). A process-hosted session is the whole process;
// every thread that works for a session binds it with MediaSessionBinding, and state is reached through
// MediaState(). The units that run on the runtime's engine thread (screenshots, WGC/DXGI sources used
// there) never reach it.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/capture/cursor_capture_state.h"
#include "common/capture/frame_queue.h"
#include "common/ipc/media_finalization.h"
#include "common/ipc/shared_defs.h"
#include "common/logging/logging.h"
#include "common/platform/atomic_shared_owner.h"
#include "mediaengine/engine/mediaengine_abi.h"

#include "inject_source_read.h"
#include "thread_scope.h"
#include "wgc_capture.h"

// Latest WGC pool/copy figures the encoder reports at shutdown. Written by the WGC path, read by the encoder.
struct WgcRuntimeLogSnapshot {
    std::atomic<bool> hasPoolEvidence{false};
    std::atomic<uint32_t> sourceFramePoolBuffers{0};
    std::atomic<uint32_t> copyPoolSlots{0};
    std::atomic<uint32_t> budgetSurfaces{0};
    std::atomic<uint32_t> syncFrames{0};
    std::atomic<uint32_t> extraFrames{0};
    std::atomic<uint32_t> retainedCap{0};
    std::atomic<uint32_t> reservedFreeSlots{0};
    std::atomic<uint32_t> safetySlots{0};
    std::atomic<uint32_t> sourceFormat{0};
    std::atomic<uint32_t> retainedFormat{0};
    std::atomic<uint32_t> compactRetained{0};
    std::atomic<uint64_t> estimatedVramBytes{0};
    std::atomic<uint64_t> sourceBudgetBytes{0};
    std::atomic<uint64_t> copyBudgetBytes{0};
    std::atomic<uint64_t> sourceSurfaceBytes{0};
    std::atomic<uint64_t> copySurfaceBytes{0};
    std::atomic<int64_t> lastConvertUs{0};
    std::atomic<uint32_t> poolLeasedMax{0};
    std::atomic<uint32_t> poolFreeMin{UINT32_MAX};
    std::atomic<uint32_t> poolSaturatedDrops{0};
    std::atomic<uint32_t> poolOverwritePrevented{0};
    std::atomic<uint32_t> poolLeaseMismatches{0};
    std::atomic<uint32_t> duplicateTimestampsSeen{0};
    std::atomic<uint32_t> duplicateTimestampsSkipped{0};

    void Reset() {
        hasPoolEvidence.store(false, std::memory_order_relaxed);
        sourceFramePoolBuffers.store(0, std::memory_order_relaxed);
        copyPoolSlots.store(0, std::memory_order_relaxed);
        budgetSurfaces.store(0, std::memory_order_relaxed);
        syncFrames.store(0, std::memory_order_relaxed);
        extraFrames.store(0, std::memory_order_relaxed);
        retainedCap.store(0, std::memory_order_relaxed);
        reservedFreeSlots.store(0, std::memory_order_relaxed);
        safetySlots.store(0, std::memory_order_relaxed);
        sourceFormat.store(0, std::memory_order_relaxed);
        retainedFormat.store(0, std::memory_order_relaxed);
        compactRetained.store(0, std::memory_order_relaxed);
        estimatedVramBytes.store(0, std::memory_order_relaxed);
        sourceBudgetBytes.store(0, std::memory_order_relaxed);
        copyBudgetBytes.store(0, std::memory_order_relaxed);
        sourceSurfaceBytes.store(0, std::memory_order_relaxed);
        copySurfaceBytes.store(0, std::memory_order_relaxed);
        lastConvertUs.store(0, std::memory_order_relaxed);
        poolLeasedMax.store(0, std::memory_order_relaxed);
        poolFreeMin.store(UINT32_MAX, std::memory_order_relaxed);
        poolSaturatedDrops.store(0, std::memory_order_relaxed);
        poolOverwritePrevented.store(0, std::memory_order_relaxed);
        poolLeaseMismatches.store(0, std::memory_order_relaxed);
        duplicateTimestampsSeen.store(0, std::memory_order_relaxed);
        duplicateTimestampsSkipped.store(0, std::memory_order_relaxed);
    }
};

struct MediaSessionState {
    // Control: recording lifecycle and mode flags.
    std::atomic<bool> running{true};
    std::atomic<bool> recording{false};
    // Latched by StartRecording. The media process is disposable - one per recording - so "this process is
    // stopping and never reached a live recording" is exactly the aborted-start case that used to exit
    // silently and leave the overlay on "Finalizing recording...".
    std::atomic<bool> recordingEverStarted{false};
    std::atomic<bool> liveStreamRecording{false};
    std::atomic<bool> recordingUsesVfr{false};
    std::atomic<bool> drainOutstandingCfrTicks{false};
    std::atomic<bool> privacyFailClosedStopRequested{false};
    std::atomic<int64_t> cfrDrainStopQpc{0};
    std::atomic<bool> useScreenGrab{false};     // Active capture mode for the current recording
    std::atomic<bool> preferScreenGrab{false};  // Preferred mode for the next recording
    // Audio-only recording flag (set via IPC or shared memory).
    bool audioOnly = false;
    std::atomic<bool> autoWgcFallbackArmed{false};
    std::string recordingManifestLogPath;
    // Diagnostic recording id (--recording-id) and the log sink this session's threads write to. The
    // process host copies the id from the command line and leaves the sink null (the process log).
    std::string recordingId;
    std::shared_ptr<ce::log::Sink> logSink;
    // The media pipeline of this session (MediaEngine_Create); 0 when none exists.
    MediaEngineHandle engine{};
    // Whether the pipeline's log lines are written (the debug log level), as the process sink is gated.
    std::atomic<bool> engineLogEnabled{false};

    // What this session may change in its process (DR-39), from its RoleContext: the process it owns, and whether the
    // settings' process priority class and GPU scheduling priority may be applied to it.
    bool ownsProcess = true;
    bool processTuning = true;
    // The process priority class before this session applied the settings; restored at shutdown. 0: none applied.
    DWORD priorityClassBeforeTuning = 0;
    std::atomic<bool> loggedPriorityLeftToClient{false};

    // Encoder: the encoder thread and the frame it last emitted.
    std::atomic<bool> encoderRunning{false};
    std::atomic<bool> isEncoderBottlenecked{false};
    std::thread encoderThread;
    QueuedFrame lastFrame;
    bool hasLastFrame = false;

    // Frames handed between the capture threads and the encoder.
    FrameQueue frameQueue{32};
    std::mutex standbyWgcFrameMutex;
    QueuedFrame standbyWgcFrame;
    bool hasStandbyWgcFrame = false;
    std::atomic<bool> retainStandbyWgcFrameForHandoff{false};

    // Capture: WGC and inject sources, their threads and the cursor timelines.
    ce::AtomicSharedOwner<WGCCapture> wgcCap;
    std::atomic<uint64_t> wgcSourceEpoch{0};
    std::atomic<bool> injectCaptureRunning{false};
    std::atomic<bool> injectCaptureShutdown{false};
    // Set true on StartRecording to reset inject session state.
    std::atomic<bool> injectSessionReset{true};
    std::thread injectCaptureThread;
    HANDLE injectFrameReadyEvent = NULL;
    HANDLE injectCaptureShutdownEvent = NULL;
    std::atomic<bool> wgcCaptureRunning{false};
    std::atomic<bool> wgcCaptureShutdown{false};
    std::thread wgcCaptureThread;
    std::atomic<bool> injectDeliveredFirstFrame{false};
    std::atomic<bool> rejectInjectFrames{false};
    std::atomic<uint32_t> wgcProducerTargetFps{0};
    // Preserve several seconds even with a 1000 Hz DXGI hardware pointer so normal delayed screen-grab
    // targets retain the source history they still need.
    ce::cursor::Timeline wgcCursorTimeline{8192};
    ce::cursor::Timeline injectCursorTimeline{1024};
    std::atomic<uint64_t> dxgiCursorTimelinePublished{0};
    std::mutex wgcCursorPublicationMutex;
    // Duplication embedded-cursor suppression: while duplicated frames already CONTAIN the cursor,
    // encoder-side cursor composition is suppressed to avoid a double cursor.
    std::atomic<bool> dupCursorSuppressionActive{false};
    // Inject source slots whose GPU reads the encoder still has in flight (inject_source_read.h).
    InjectSourceReadReleaseQueue injectSourceReadReleases;
    // Latched source-loss truth (capture_retarget_policy.h): folded into every health publication so no later
    // observation can undo that the requested source died mid-recording.
    std::atomic<uint32_t> sourceLossHealthFlags{0};

    // Channel views onto the hook-side shared memory (mapped at start, unmapped at shutdown).
    HANDLE hMapFile = NULL;
    SharedMemoryLayout* pSharedMem = nullptr;
    HANDLE hMapShmem = NULL;
    ShmemBuffer* pShmem = nullptr;

    // Health and diagnostics.
    std::atomic<uint32_t> recordingHealthFlags{0};
    std::atomic<uint32_t> recordingTimelineDebtMs{0};
    std::atomic<uint32_t> recordingPeakTimelineDebtMs{0};
    std::atomic<uint32_t> recordingCapacityAttributedDebtMs{0};
    std::atomic<uint64_t> injectDeferredFrames{0};
    std::atomic<uint32_t> injectBufferedTrimmedFrames{0};
    std::atomic<uint32_t> injectCadenceDroppedFrames{0};
    std::atomic<uint64_t> activePathMismatchFramesDiscarded{0};
    WgcRuntimeLogSnapshot wgcRuntimeLogSnapshot;
    std::string avSyncConfidence = "low";
    std::string avSyncReason = "not_measured";
    bool avSyncUsedAudioProbe = false;
    // Auto-detected render-endpoint audio latency, measured once per session (<0 = not measured / unavailable).
    double autoDetectedRenderLatencyMs = -1.0;
    bool renderLatencyMeasureAttempted = false;
    // Removes the legacy on-disk latency cache once per session (DeleteLegacyAudioLatencyCacheFileOnce).
    bool legacyAudioLatencyCacheCleanupAttempted = false;
    std::mutex legacyAudioLatencyCacheCleanupMutex;

    // Result: where this process reports what became of its recording (common/ipc/media_finalization.h). The
    // slot is inherited from the spawner (null without one); the latest result's coded form is the exit code.
    ce::media_result::Slot* resultSlot = nullptr;
    ce::media_result::Result lastFinalization;

    // Semantic state that used to be function-local statics. Each was correct only because a media process
    // ends with its recording; as session state it is correct for any session.
    // Recording loop (media_main_start_loop.cpp).
    bool lastUseEncoderTextures = false;
    uint32_t localReadIdx = 0;
    bool receivedFirstFrame = false;
    DWORD injectModeStartTime = 0;
    bool sessionInitialized = false;
    bool sharedTexturesCreated = false;
    // Encoder loop (media_main_encoder_05a/05c/09).
    uint64_t retiredPullFrameDrops = 0;
    int64_t lastWgcSrcQpc = 0;
    uint64_t freshEncodeRecoveryCount = 0;
    uint32_t wgcCapacityLimitedStreakSeconds = 0;
    // Inject and WGC threads (media_main_threads_inject.cpp, media_main_wgc.cpp, inject_source_read.cpp).
    int64_t lastInjectTimestamp = 0;
    uint64_t staleTransportDrops = 0;
    std::atomic<uint32_t> mismatchCount{0};
    DWORD cursorWindowPid = 0;
    HWND cursorWindow = NULL;
    std::atomic<int64_t> lastWgcTimestamp{0};
    std::atomic<uint64_t> staleEpochDrops{0};
    std::atomic<uint32_t> retainedWithRead{0};
    std::atomic<bool> firstDeferLogged{false};
    // Frame submission outcome counters (frame_submission_internal.h).
    std::atomic<uint64_t> acceptedFreshOutputs{0};
    std::atomic<uint64_t> acceptedTotalOutputs{0};
};

// The session the calling thread works for: the one its MediaSessionBinding installed, else the process
// session MediaProcessMain sets. Neither exists on a thread outside the media role; that is a programming
// error, which logs once and terminates.
MediaSessionState& MediaState();

// Binds `state` to the calling thread until destroyed; nested bindings restore the outer one. The state's log
// sink is bound with it (a null sink keeps the process log).
class MediaSessionBinding {
public:
    explicit MediaSessionBinding(MediaSessionState& state);
    ~MediaSessionBinding();
    MediaSessionBinding(const MediaSessionBinding&) = delete;
    MediaSessionBinding& operator=(const MediaSessionBinding&) = delete;

private:
    MediaSessionState* previous_;
    ce::log::SinkScope sinkScope_;
};

// Process host only (MediaProcessMain): the session every thread without a binding uses. The helper process
// owns exactly one session, so threads that were never bound behave as they always did.
class MediaProcessDefaultScope {
public:
    explicit MediaProcessDefaultScope(MediaSessionState& state);
    ~MediaProcessDefaultScope();
    MediaProcessDefaultScope(const MediaProcessDefaultScope&) = delete;
    MediaProcessDefaultScope& operator=(const MediaProcessDefaultScope&) = delete;

private:
    MediaSessionState* previous_;
};

// Thread scope that binds `state` around each body it runs (for shared units, see thread_scope.h).
ce::ThreadScope MediaSessionScope(MediaSessionState& state);
