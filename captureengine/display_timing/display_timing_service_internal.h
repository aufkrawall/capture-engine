#pragma once

// Private to the display-timing service units: the service object shared by
// display_timing_service.cpp (session lifecycle, draining, health) and
// display_timing_service_events.cpp (the ETW event reducers).

#include "display_timing_service.h"
#include "display_timing_composed.h"
#include "display_timing_compositor.h"
#include "display_timing_correlation.h"
#include "display_timing_etw.h"
#include "display_timing_health.h"
#include "display_timing_input.h"
#include "display_timing_intervals.h"
#include "display_timing_nvidia.h"
#include "display_timing_policy.h"
#include "display_timing_publication.h"
#include "display_timing_refresh.h"
#include "display_timing_session_reclaim.h"
#include "display_timing_startup.h"
#include "captureengine/elevation/elevation_client.h"
#include "display_timing_submissions.h"
#include "display_timing_vblank.h"

#include <windows.h>
#include <evntrace.h>
#include <tdh.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "common/ipc/display_timing_shared.h"
#include "common/logging/logging.h"

class DisplayTimingService::Impl {
public:
    ~Impl() noexcept {
        StopNoexcept();
    }

    void Start();
    void UpdateTargets(const std::vector<DisplayTimingTarget>& targets);
    bool NeedsRestart() const;

private:

    static void WINAPI EventRecordThunk(EVENT_RECORD* event);
    static ULONG WINAPI BufferThunk(EVENT_TRACE_LOGFILEW* trace);
    void SetStartupFailure(ULONG error);
    void ObserveTraceLosses(ULONG eventsLost);
    bool IsTrackedProcess(uint32_t processId) const;
    void HandleEvent(EVENT_RECORD* event);

    // The announcement carries the time the driver scheduled the flip for, which
    // is the only screen time available while frame generation paces several
    // flips out of one render. The provider has no registered manifest, so the
    // payload is read positionally and the field is located by value; see
    // display_timing_nvidia.h.
    void HandleNvidiaFlipRequest(EVENT_RECORD* event);
    void HandleGraphicsKernelEvent(EVENT_RECORD* event);
    void HandleQueuePacket(EVENT_RECORD* event);

    // A composed process's present packet finishing is its surface becoming
    // ready for the compositor. The event is not attributed to the submitting
    // process, so the submit sequence alone identifies it.
    void HandleQueuePacketStop(EVENT_RECORD* event);

    // A compositor flip shows the newest composed frame that was ready when
    // the compositor submitted it; see display_timing_composed.h.
    void PublishComposedFrame(int64_t compositorSubmitTimestamp, int64_t timestamp, uint32_t displaySource);

    // Every vertical blank is observed, whether or not it carries a flip of a
    // tracked process. This is diagnostic only: HSync/VSync flip completions
    // below keep their original timestamps, including genuine uneven pacing.
    void HandleVsync(EVENT_RECORD* event);
    void HandleMpoSync(EVENT_RECORD* event);
    void HandleMpoPresentIds(EVENT_RECORD* event);
    void HandleGeneratedFlip(EVENT_RECORD* event);
    void HandleImmediateFlip(EVENT_RECORD* event);
    void HandleImmediateMpoFlip(EVENT_RECORD* event);
    void HandleInputRetrieval(EVENT_RECORD* event);
    void PublishInputBurst(const DisplayInputRetrievalBursts::Burst& burst);
    void PublishForSubmit(uint32_t submitSequence, int64_t timestamp, DisplayCompletionKind completionKind,
                          bool erase, DisplayCompletionSource source, uint32_t displaySource = 0);
    void ConsumeCorrelationPayloads();
    void QueueTimestamp(uint32_t processId, uint64_t associationId, int64_t timestamp,
                        DisplayCompletionKind completionKind, int64_t presentStartTimestamp,
                        uint32_t displaySource = 0, bool synchronizedFlip = false);
    bool ShouldPublish(const PendingTimestamp& pending) const;
    void SortPending() noexcept;
    void DrainReady(int64_t nowQpc, bool force);

    // Destruction happens after both ETW workers have stopped.  Do not run the
    // normal fallback commit path here: CommitFallback may grow an unordered
    // map and therefore cannot be part of a non-throwing destructor cleanup.
    void DrainReadyNoexcept(int64_t nowQpc) noexcept;

    // Submissions expire at the completion bound (plus the reorder window a
    // late-delivered completion may still need), not with the 10 s payload
    // maps: an entry that outlives its bound can only be claimed wrongly.
    void PruneSubmissions(int64_t nowQpc);
    void PruneAssociations(int64_t cutoff);

    // Source provenance is independent of how even the measured intervals are.
    static bool IsScreenTime(const PendingTimestamp& pending);
    void PublishPending(const PendingTimestamp& pending, int64_t publishUs);

    // Queried outside the lock: QueryDisplayConfig can take a while and the
    // ETW callback must not wait on it. Logged only when the table changes.
    void RefreshDisplayPeriods();

    // Returns false while the window is not due, so the caller stays a one-liner.
    bool SnapshotHealth(DisplayTimingHealth& health);
    void SnapshotIntervals(DisplayTimingHealth& health);
    void LogHealthIfDue();
    void FlushLoop();
    void StopTraceSession();
    void StopNoexcept() noexcept;

    std::atomic<bool> started_{false};
    std::atomic<DisplayTimingStatus> startupStatus_{DisplayTimingStatus::Unavailable};
    TRACEHANDLE session_ = 0;
    ce::elevation::Client broker_;
    bool brokerConfigured_ = false;
    bool brokerTrace_ = false;
    uint32_t attemptedServicePid_ = 0;
    std::atomic<uint32_t> deniedServicePid_{0};
    std::atomic<bool> consuming_{false};
    TRACEHANDLE traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    HANDLE stopEvent_ = nullptr;
    wchar_t sessionName_[display_timing_etw::kTraceSessionNameCapacity] = {};
    int64_t qpcFrequency_ = 0;
    int64_t lastPruneQpc_ = 0;
    int64_t lastSubmissionPruneQpc_ = 0;
    DisplaySubmissionExpiryMonitor expiryMonitor_;
    std::thread processThread_;
    std::thread flushThread_;

    std::mutex mutex_;
    std::vector<DisplayTimingTarget> targets_;
    DisplaySubmissionTracker submissions_;
    DisplayTimingCorrelation correlation_;
    NvidiaFlipSchedule nvidiaSchedule_;
    ComposedPresentation composed_;
    uint64_t ownCompletions_ = 0;
    VerticalBlankClock verticalBlanks_;
    std::vector<PendingTimestamp> pendingTimestamps_;
    DisplayTimingOutputs outputs_;
    DisplayInputRetrievalBursts inputBursts_;
    uint32_t inputLoggedPid_ = 0;
    DisplayRefreshPeriods refreshPeriods_;
    uint64_t lastRefreshQueryTime_ = 0;
    bool refreshPeriodsLogged_ = false;
    DisplayIntervalStats runtimeIntervals_;
    uint32_t runtimeIntervalPid_ = 0;
    DisplayIntervalStats blankIntervals_;
    DisplayIntervalStats latchIntervals_;
    uint64_t nextTimestampOrder_ = 1;
    ULONG observedTraceEventsLost_ = 0;
    ULONG loggedTraceEventsLost_ = 0;
    uint64_t lastTraceLossLogTime_ = 0;
    uint64_t lastHealthLogTime_ = 0;
    uint64_t queuedTimestamps_ = 0;
    uint64_t suppressedTimestamps_ = 0;
    uint64_t frameTypePayloadReceived_ = 0;
    uint64_t frameTypePayloadValid_ = 0;
    uint64_t frameTypeCorrelated_ = 0;
    uint64_t frameTypeAuthoritative_ = 0;
    uint64_t frameTypePendingObserved_ = 0;
    uint64_t frameTypePayloadDuplicate_ = 0;
    uint64_t frameTypePayloadLate_ = 0;
    uint64_t fallbackPublished_ = 0;
    uint64_t fallbackSuppressed_ = 0;
    std::array<uint64_t, static_cast<std::size_t>(DisplayCompletionSource::Count)> completionsBySource_ = {};
};
