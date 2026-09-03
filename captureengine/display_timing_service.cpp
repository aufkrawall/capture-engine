#include "display_timing_service.h"
#include "display_timing_correlation.h"
#include "display_timing_etw.h"
#include "display_timing_health.h"
#include "display_timing_intervals.h"
#include "display_timing_nvidia.h"
#include "display_timing_policy.h"
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
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common/display_timing_shared.h"
#include "../common/logging.h"

using namespace display_timing_etw;

namespace {
constexpr int64_t kTimestampReorderWindowUs = 24'000;
constexpr uint64_t kHealthLogPeriodMs = 10'000;
constexpr DWORD kTraceFlushPeriodMs = 8;

}  // namespace

class DisplayTimingService::Impl {
public:
    ~Impl() noexcept {
        StopNoexcept();
    }

    void Start() {
        if (started_.exchange(true, std::memory_order_acq_rel))
            return;

        LARGE_INTEGER frequency = {};
        QueryPerformanceFrequency(&frequency);
        qpcFrequency_ = frequency.QuadPart;
        nvidiaAnnouncements_.SetQpcFrequency(qpcFrequency_);
        swprintf(sessionName_, std::size(sessionName_), L"CE_DisplayTiming_%08X", GetCurrentProcessId());
        auto properties = MakeProperties(sessionName_);

        ULONG status = StartTraceW(&session_, sessionName_, properties.Get());
        if (status != ERROR_SUCCESS) {
            SetStartupFailure(status);
            return;
        }

        status = EnableFilteredProvider(session_, kRuntimeProvider, kRuntimeKeyword,
                                        {kRuntimePresentStart, kRuntimeMpoPresentStart});
        if (status == ERROR_SUCCESS) {
            status = EnableFilteredProvider(session_, kGraphicsKernelProvider, kGraphicsKernelKeyword,
                                            {kQueuePacketStart, kMmioFlip, kMmioMpoFlip, kVsync, kVsyncMpo,
                                             kHsyncMpo, kMpoPresentIds});
        }
        if (status == ERROR_SUCCESS) {
            const ULONG optionalStatus =
                EnableFilteredProvider(session_, kFrameTypeProvider, kFrameTypeKeyword, {kGeneratedFlip});
            if (optionalStatus != ERROR_SUCCESS) {
                LogWarn("[DisplayTiming] Generated-frame timestamp events are unavailable: %lu", optionalStatus);
            }
            // Optional like the frame-type provider: absent on non-NVIDIA
            // adapters, where flip event timestamps already are the screen times.
            const ULONG nvidiaStatus = EnableFilteredProvider(session_, kNvidiaDisplayProvider, kNvidiaDisplayKeyword,
                                                             {kNvidiaFlipRequest});
            if (nvidiaStatus != ERROR_SUCCESS) {
                LogWarn("[DisplayTiming] NVIDIA scheduled-flip announcements are unavailable: %lu", nvidiaStatus);
            }
        }
        if (status != ERROR_SUCCESS) {
            SetStartupFailure(status);
            StopTraceSession();
            return;
        }

        EVENT_TRACE_LOGFILEW trace = {};
        trace.LoggerName = sessionName_;
        trace.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                                 PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        trace.EventRecordCallback = &EventRecordThunk;
        trace.BufferCallback = &BufferThunk;
        trace.Context = this;
        traceHandle_ = OpenTraceW(&trace);
        if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
            SetStartupFailure(GetLastError());
            StopTraceSession();
            return;
        }

        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent_) {
            SetStartupFailure(GetLastError());
            CloseTrace(traceHandle_);
            traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
            StopTraceSession();
            return;
        }

        startupStatus_.store(DisplayTimingStatus::Starting, std::memory_order_release);
        processThread_ = std::thread([this] {
            const ULONG traceStatus = ProcessTrace(&traceHandle_, 1, nullptr, nullptr);
            // ERROR_CANCELLED is the ordinary result of stopping the session.
            if (traceStatus != ERROR_SUCCESS && traceStatus != ERROR_CANCELLED) {
                startupStatus_.store(DisplayTimingStatus::Failed, std::memory_order_release);
                LogWarn("[DisplayTiming] Event consumption stopped: %lu", traceStatus);
            }
        });
        flushThread_ = std::thread([this] { FlushLoop(); });
        LogInfo("[DisplayTiming] Screen-change timing service started (flush=%lums reorder=%lldus)",
                kTraceFlushPeriodMs, static_cast<long long>(kTimestampReorderWindowUs));
    }

    void UpdateTargets(const std::vector<DisplayTimingTarget>& targets) {
        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& oldTarget : targets_) {
            const bool retained = std::any_of(targets.begin(), targets.end(), [&](const DisplayTimingTarget& target) {
                return target.output == oldTarget.output && target.sourcePid == oldTarget.sourcePid &&
                       target.rendererPid == oldTarget.rendererPid;
            });
            if (!retained && oldTarget.output) {
                oldTarget.output->Reset(0, 0, DisplayTimingStatus::Unavailable);
                lastPublishedByOutput_.erase(oldTarget.output);
            }
        }

        for (const auto& target : targets) {
            const bool unchanged = std::any_of(targets_.begin(), targets_.end(), [&](const DisplayTimingTarget& old) {
                return target.output == old.output && target.sourcePid == old.sourcePid &&
                       target.rendererPid == old.rendererPid;
            });
            if (!unchanged && target.output) {
                target.output->Reset(target.sourcePid, target.rendererPid,
                                     startupStatus_.load(std::memory_order_acquire));
                lastPublishedByOutput_.try_emplace(target.output);
            }
        }
        targets_ = targets;
    }

private:

    static void WINAPI EventRecordThunk(EVENT_RECORD* event) {
        static_cast<Impl*>(event->UserContext)->HandleEvent(event);
    }

    static ULONG WINAPI BufferThunk(EVENT_TRACE_LOGFILEW* trace) {
        auto* self = static_cast<Impl*>(trace->Context);
        self->ObserveTraceLosses(trace->EventsLost);
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        self->DrainReady(now.QuadPart, false);
        return TRUE;
    }

    void SetStartupFailure(ULONG error) {
        const DisplayTimingStatus status =
            error == ERROR_ACCESS_DENIED ? DisplayTimingStatus::AccessDenied : DisplayTimingStatus::Failed;
        startupStatus_.store(status, std::memory_order_release);
        if (error == ERROR_ACCESS_DENIED) {
            LogWarn("[DisplayTiming] Screen-change timing is unavailable (access denied); overlays will use "
                    "presentation timing");
        } else {
            LogWarn("[DisplayTiming] Screen-change timing startup failed: %lu; overlays will use presentation timing",
                    error);
        }
    }

    void ObserveTraceLosses(ULONG eventsLost) {
        ULONG loggedTotal = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (eventsLost > observedTraceEventsLost_) {
                const ULONG newlyLost = eventsLost - observedTraceEventsLost_;
                observedTraceEventsLost_ = eventsLost;
                for (const auto& target : targets_) {
                    if (target.output)
                        target.output->droppedTimestampCount.fetch_add(newlyLost, std::memory_order_relaxed);
                }
            }
            const uint64_t now = GetTickCount64();
            if (observedTraceEventsLost_ > loggedTraceEventsLost_ &&
                (lastTraceLossLogTime_ == 0 || now - lastTraceLossLogTime_ >= 10000)) {
                loggedTraceEventsLost_ = observedTraceEventsLost_;
                lastTraceLossLogTime_ = now;
                loggedTotal = loggedTraceEventsLost_;
            }
        }
        if (loggedTotal != 0)
            LogWarn("[DisplayTiming] Graphics event loss detected: total=%lu", loggedTotal);
    }

    bool IsTrackedProcess(uint32_t processId) const {
        return std::any_of(targets_.begin(), targets_.end(), [&](const DisplayTimingTarget& target) {
            return target.sourcePid == processId || (target.rendererPid != 0 && target.rendererPid == processId);
        });
    }

    void HandleEvent(EVENT_RECORD* event) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& header = event->EventHeader;
        if (IsEqualGUID(header.ProviderId, kRuntimeProvider)) {
            if ((header.EventDescriptor.Id == kRuntimePresentStart ||
                 header.EventDescriptor.Id == kRuntimeMpoPresentStart) &&
                IsTrackedProcess(header.ProcessId)) {
                submissions_.ObserveRuntimePresent(header.ProcessId, header.ThreadId,
                                                   header.TimeStamp.QuadPart);
                // The same frames the published series is built from, measured
                // one stage earlier. Several tracked processes would interleave
                // into one meaningless series, so the accumulator follows the
                // most recent one instead of mixing them.
                if (runtimeIntervalPid_ != header.ProcessId) {
                    runtimeIntervalPid_ = header.ProcessId;
                    runtimeIntervals_.StartWindow();
                }
                runtimeIntervals_.Observe(DisplayTimingQpcToUs(header.TimeStamp.QuadPart, qpcFrequency_));
            }
            return;
        }

        if (IsEqualGUID(header.ProviderId, kGraphicsKernelProvider)) {
            HandleGraphicsKernelEvent(event);
            return;
        }

        if (IsEqualGUID(header.ProviderId, kNvidiaDisplayProvider) &&
            header.EventDescriptor.Id == kNvidiaFlipRequest) {
            HandleNvidiaFlipRequest(event);
            return;
        }

        if (IsEqualGUID(header.ProviderId, kFrameTypeProvider) && header.EventDescriptor.Id == kGeneratedFlip)
            HandleGeneratedFlip(event);
    }

    // The announcement carries the time the driver scheduled the flip for, which
    // is the only screen time available while frame generation paces several
    // flips out of one render. The provider has no registered manifest, so the
    // payload is read positionally and the field is located by value; see
    // display_timing_nvidia.h.
    void HandleNvidiaFlipRequest(EVENT_RECORD* event) {
        ++nvidiaAnnouncementsReceived_;
        const int64_t eventQpc = event->EventHeader.TimeStamp.QuadPart;
        const int64_t announcedQpc =
            nvidiaAnnouncements_.Decode(event->UserData, event->UserDataLength, eventQpc);
        if (announcedQpc == 0) {
            ++nvidiaAnnouncementsUndecodable_;
            return;
        }
        nvidiaFlips_.ObserveAnnouncement(event->EventHeader.ThreadId, eventQpc, announcedQpc);
    }

    void HandleGraphicsKernelEvent(EVENT_RECORD* event) {
        const auto& header = event->EventHeader;
        switch (header.EventDescriptor.Id) {
            case kQueuePacketStart:
                HandleQueuePacket(event);
                break;
            case kVsync:
                HandleVsync(event);
                break;
            case kVsyncMpo:
            case kHsyncMpo:
                HandleMpoSync(event);
                break;
            case kMpoPresentIds:
                HandleMpoPresentIds(event);
                break;
            case kMmioFlip:
                HandleImmediateFlip(event);
                break;
            case kMmioMpoFlip:
                HandleImmediateMpoFlip(event);
                break;
            default:
                break;
        }
    }

    void HandleQueuePacket(EVENT_RECORD* event) {
        uint32_t submitSequence = 0;
        uint32_t isPresent = 0;
        if (!ReadProperty(event, L"SubmitSequence", submitSequence) ||
            !ReadProperty(event, L"bPresent", isPresent) || isPresent == 0) {
            return;
        }
        submissions_.Associate(event->EventHeader.ProcessId, event->EventHeader.ThreadId, submitSequence,
                               event->EventHeader.TimeStamp.QuadPart);
    }

    // Every vertical blank is observed, whether or not it carries a flip of a
    // tracked process: this event is the screen's own clock, and the deferred
    // flip completions below are rounded onto it.
    void HandleVsync(EVENT_RECORD* event) {
        uint32_t displaySource = 0;
        if (ReadProperty(event, L"VidPnSourceId", displaySource))
            verticalBlanks_.Observe(displaySource, event->EventHeader.TimeStamp.QuadPart);
        uint64_t fenceId = 0;
        if (!ReadProperty(event, L"FlipFenceId", fenceId) || fenceId == 0)
            return;
        PublishForSubmit(static_cast<uint32_t>(fenceId >> 32u), event->EventHeader.TimeStamp.QuadPart,
                         DisplayCompletionKind::Sync, true, DisplayCompletionSource::VSyncDpc, displaySource);
    }

    void HandleMpoSync(EVENT_RECORD* event) {
        uint32_t count = 0;
        if (!ReadProperty(event, L"FlipEntryCount", count) || count == 0 || count > 64)
            return;
        const DisplayCompletionSource source = event->EventHeader.EventDescriptor.Id == kHsyncMpo
                                                   ? DisplayCompletionSource::HSyncDpcMultiPlane
                                                   : DisplayCompletionSource::VSyncDpcMultiPlane;
        uint32_t displaySource = 0;
        ReadProperty(event, L"VidPnSourceId", displaySource);
        std::array<uint32_t, 64> publishedPids = {};
        std::size_t publishedCount = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t encodedSequence = 0;
            if (!ReadProperty(event, L"FlipSubmitSequence", encodedSequence, i) || encodedSequence == 0)
                continue;
            const uint32_t submitSequence = static_cast<uint32_t>(encodedSequence >> 32u);
            const SubmitAssociation* association = submissions_.Find(submitSequence);
            if (!association)
                continue;
            const uint32_t processId = association->processId;
            if (std::find(publishedPids.begin(), publishedPids.begin() + publishedCount, processId) ==
                publishedPids.begin() + publishedCount) {
                QueueTimestamp(processId, association->associationId, event->EventHeader.TimeStamp.QuadPart,
                               DisplayCompletionKind::Sync, association->presentStartTimestamp, displaySource);
                ++completionsBySource_[static_cast<std::size_t>(source)];
                publishedPids[publishedCount++] = processId;
            }
            submissions_.Erase(submitSequence);
        }
    }

    void HandleMpoPresentIds(EVENT_RECORD* event) {
        if (event->EventHeader.EventDescriptor.Version < 8)
            return;
        uint32_t displaySource = 0;
        uint32_t planeCount = 0;
        uint32_t submitSequence = 0;
        if (!ReadProperty(event, L"VidPnSourceId", displaySource) ||
            !ReadProperty(event, L"PlaneCount", planeCount) ||
            !ReadProperty(event, L"FlipSubmitSequence", submitSequence) || planeCount == 0 || planeCount > 64) {
            return;
        }
        const SubmitAssociation* association = submissions_.Find(submitSequence);
        if (!association)
            return;
        for (uint32_t i = 0; i < planeCount; ++i) {
            uint64_t presentId = 0;
            uint32_t layer = 0;
            if (ReadProperty(event, L"PresentId", presentId, i) && ReadProperty(event, L"LayerIndex", layer, i)) {
                const DisplayLayerPresentKey layerKey = {displaySource, layer, presentId};
                correlation_.Associate(layerKey, {association->processId, association->associationId,
                                                  event->EventHeader.TimeStamp.QuadPart,
                                                  association->presentStartTimestamp});
                ConsumeCorrelationPayloads();
            }
        }
    }

    void HandleGeneratedFlip(EVENT_RECORD* event) {
        ++frameTypePayloadReceived_;
        const uint8_t version = event->EventHeader.EventDescriptor.Version;
        if (version > 1)
            return;

        uint32_t displaySource = 0;
        uint32_t layer = 0;
        uint64_t presentId = 0;
        uint8_t frameType = 0;
        if (!ReadProperty(event, L"VidPnSourceId", displaySource) ||
            !ReadProperty(event, L"LayerIndex", layer) || !ReadProperty(event, L"PresentId", presentId) ||
            !ReadProperty(event, L"FrameType", frameType)) {
            return;
        }

        uint64_t screenTime = static_cast<uint64_t>(event->EventHeader.TimeStamp.QuadPart);
        if (version == 1 && !ReadProperty(event, L"TimeStamp", screenTime))
            return;
        if (screenTime == 0)
            return;

        ++frameTypePayloadValid_;
        const DisplayLayerPresentKey layerKey = {displaySource, layer, presentId};
        const auto result = correlation_.ObservePayload(
            layerKey, DisplayPendingFrameTypeFlip{static_cast<int64_t>(screenTime),
                                                   event->EventHeader.TimeStamp.QuadPart, frameType});
        ConsumeCorrelationPayloads();
        if (result == DisplayTimingCorrelation::PayloadResult::Duplicate)
            ++frameTypePayloadDuplicate_;
        else if (result == DisplayTimingCorrelation::PayloadResult::Late)
            ++frameTypePayloadLate_;
        else if (result == DisplayTimingCorrelation::PayloadResult::Pending)
            ++frameTypePendingObserved_;
    }

    void HandleImmediateFlip(EVENT_RECORD* event) {
        uint32_t submitSequence = 0;
        uint32_t flags = 0;
        if (ReadProperty(event, L"FlipSubmitSequence", submitSequence) && ReadProperty(event, L"Flags", flags) &&
            (flags & 2u) != 0) {
            PublishForSubmit(submitSequence, event->EventHeader.TimeStamp.QuadPart,
                             DisplayCompletionKind::Immediate, true, DisplayCompletionSource::ImmediateFlip);
        }
    }

    void HandleImmediateMpoFlip(EVENT_RECORD* event) {
        if (event->EventHeader.EventDescriptor.Version < 2)
            return;
        uint64_t encodedSequence = 0;
        uint32_t status = 0;
        if (!ReadProperty(event, L"FlipSubmitSequence", encodedSequence) ||
            !ReadProperty(event, L"FlipEntryStatusAfterFlip", status)) {
            return;
        }
        if (status == kFlipWaitVSync || status == kFlipWaitHSync) {
            // The matching ?SyncDPC event completes this flip. Its announcement
            // is still consumed rather than left behind: measured under FSR
            // frame generation on this hardware it leads this flip event by
            // about two microseconds, so it says nothing the completion does
            // not, while an announcement stranded here would later be applied
            // to an unrelated immediate flip on the same driver thread.
            nvidiaFlips_.TakeFlipDelay(event->EventHeader.ThreadId);
            return;
        }
        // Consume the announcement on every immediate flip, not only on the ones
        // that resolve to a tracked process: an announcement left behind by a
        // flip we do not publish would otherwise be applied to an unrelated
        // later flip on the same driver thread.
        const NvidiaFlipDelay announced = nvidiaFlips_.TakeFlipDelay(event->EventHeader.ThreadId);
        if (announced.matched) {
            ++nvidiaAnnouncementsApplied_;
            nvidiaAnnouncedDelayTotal_ += announced.delay;
            nvidiaAnnouncedDelayMax_ = std::max(nvidiaAnnouncedDelayMax_, announced.delay);
        }
        PublishForSubmit(static_cast<uint32_t>(encodedSequence >> 32u),
                         event->EventHeader.TimeStamp.QuadPart + announced.delay,
                         DisplayCompletionKind::Immediate, true, DisplayCompletionSource::ImmediateMultiPlaneFlip);
    }

    void PublishForSubmit(uint32_t submitSequence, int64_t timestamp, DisplayCompletionKind completionKind,
                          bool erase, DisplayCompletionSource source, uint32_t displaySource = 0) {
        const SubmitAssociation* association = submissions_.Find(submitSequence);
        if (!association)
            return;
        QueueTimestamp(association->processId, association->associationId, timestamp, completionKind,
                       association->presentStartTimestamp, displaySource);
        ++completionsBySource_[static_cast<std::size_t>(source)];
        if (erase)
            submissions_.Erase(submitSequence);
    }

    void ConsumeCorrelationPayloads() {
        auto payloads = correlation_.TakePayloads();
        for (auto& payload : payloads) {
            pendingTimestamps_.push_back(payload);
            ++queuedTimestamps_;
            // This is the single transition point for both delivery orders:
            // payload-first becomes matched when MPO association consumes it,
            // while MPO-first reaches here immediately from ObservePayload.
            ++frameTypeCorrelated_;
            ++frameTypeAuthoritative_;
        }
    }

    void QueueTimestamp(uint32_t processId, uint64_t associationId, int64_t timestamp,
                        DisplayCompletionKind completionKind, int64_t presentStartTimestamp,
                        uint32_t displaySource = 0) {
        if (timestamp <= 0)
            return;
        if (completionKind != DisplayCompletionKind::Unconditional) {
            // The fallback itself owns the association tombstone.  This makes
            // a later FrameType telemetry-only even when no payload preceded
            // the fallback (the 24 ms watermark is a bounded policy, not a
            // causal/no-late-events guarantee).
            correlation_.QueueFallback(processId, associationId, timestamp, completionKind,
                                       pendingTimestamps_, nextTimestampOrder_, presentStartTimestamp,
                                       displaySource);
            ++queuedTimestamps_;
            return;
        }
        pendingTimestamps_.push_back({processId, associationId, timestamp, completionKind,
                                      nextTimestampOrder_++, presentStartTimestamp, displaySource});
        ++queuedTimestamps_;
    }

    bool ShouldPublish(const PendingTimestamp& pending) const {
        return correlation_.ShouldPublish(pending);
    }

    void SortPending() noexcept {
        std::sort(pendingTimestamps_.begin(), pendingTimestamps_.end(), [](const auto& a, const auto& b) {
            return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.arrivalOrder < b.arrivalOrder;
        });
    }

    // A flip that waited for a vertical blank changed the screen at that blank,
    // never at the moment the DPC reported it: on the hardware flip queue the
    // driver latches each flip a variable time ahead of the scanout that shows
    // it, which under frame generation alternates and turns a steady cadence
    // into a sawtooth. Rounding onto the blank clock is skipped, not guessed,
    // while the clock cannot answer.
    void ResolveDeferredScreenTimes() {
        blankAdjustedTimestamps_ += ::ResolveDeferredScreenTimes(pendingTimestamps_, verticalBlanks_);
    }

    void DrainReady(int64_t nowQpc, bool force) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lastPruneQpc_ == 0 || nowQpc - lastPruneQpc_ >= qpcFrequency_ * 5) {
            PruneAssociations(nowQpc - qpcFrequency_ * 10);
            lastPruneQpc_ = nowQpc;
        }
        if (pendingTimestamps_.empty())
            return;
        SortPending();
        // Blanks are claimed in the order the frames reach the screen, so the
        // queue is ordered first and re-ordered afterwards: resolving moves a
        // completion forward onto its blank, by less than one refresh in the
        // ordinary case but never by a fixed amount.
        ResolveDeferredScreenTimes();
        SortPending();
        const int64_t cutoff = nowQpc - (kTimestampReorderWindowUs * qpcFrequency_) / 1'000'000;
        const int64_t publishUs = DisplayTimingQpcToUs(nowQpc, qpcFrequency_);
        std::size_t consumed = 0;
        for (auto& pending : pendingTimestamps_) {
            if (!force && pending.timestamp > cutoff)
                break;
            // Publishing a deferred completion the blank clock never answered
            // for means the uncorrected timestamp reaches the overlay, which is
            // the one thing this counter has to make visible.
            if (pending.completionKind == DisplayCompletionKind::Sync && !pending.screenTimeResolved)
                ++blankUnresolvedTimestamps_;
            if (ShouldPublish(pending)) {
                PublishTimestamp(pending.processId, pending.timestamp, publishUs,
                                 pending.presentStartTimestamp);
                if (pending.completionKind != DisplayCompletionKind::Unconditional) {
                    ++fallbackPublished_;
                    correlation_.CommitFallback(pending);
                }
            } else {
                ++suppressedTimestamps_;
                if (pending.completionKind != DisplayCompletionKind::Unconditional)
                    ++fallbackSuppressed_;
            }
            ++consumed;
        }
        pendingTimestamps_.erase(
            pendingTimestamps_.begin(),
            pendingTimestamps_.begin() + static_cast<std::vector<PendingTimestamp>::difference_type>(consumed));
    }

    // Destruction happens after both ETW workers have stopped.  Do not run the
    // normal fallback commit path here: CommitFallback may grow an unordered
    // map and therefore cannot be part of a non-throwing destructor cleanup.
    void DrainReadyNoexcept(int64_t nowQpc) noexcept {
        if (pendingTimestamps_.empty())
            return;
        SortPending();
        ResolveDeferredScreenTimes();
        SortPending();
        const int64_t publishUs = DisplayTimingQpcToUs(nowQpc, qpcFrequency_);
        for (const auto& pending : pendingTimestamps_)
            if (ShouldPublish(pending))
                PublishTimestamp(pending.processId, pending.timestamp, publishUs,
                                 pending.presentStartTimestamp);
        pendingTimestamps_.clear();
    }

    void PruneAssociations(int64_t cutoff) {
        submissions_.PruneBefore(cutoff);
        correlation_.Prune(cutoff);
        nvidiaFlips_.PruneBefore(cutoff);
    }

    void PublishTimestamp(uint32_t processId, int64_t timestamp, int64_t publishUs,
                          int64_t presentStartTimestamp) {
        for (const auto& target : targets_) {
            if (!target.output)
                continue;
            if (target.sourcePid != processId && target.rendererPid != processId)
                continue;
            const auto lastPublished = lastPublishedByOutput_.find(target.output);
            if (lastPublished == lastPublishedByOutput_.end())
                continue;
            if (timestamp <= lastPublished->second.lastTimestamp) {
                target.output->droppedTimestampCount.fetch_add(1, std::memory_order_relaxed);
                ++regressedTimestamps_;
                continue;
            }
            lastPublished->second.lastTimestamp = timestamp;
            const int64_t screenTimeUs = DisplayTimingQpcToUs(timestamp, qpcFrequency_);
            lastPublished->second.intervals.Observe(screenTimeUs);
            target.output->Publish(screenTimeUs, publishUs,
                                   DisplayTimingQpcToUs(presentStartTimestamp, qpcFrequency_));
            ++publishedTimestamps_;
        }
    }

    // Returns false while the window is not due, so the caller stays a one-liner.
    bool SnapshotHealth(DisplayTimingHealth& health) {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t now = GetTickCount64();
        if (targets_.empty() || (lastHealthLogTime_ != 0 && now - lastHealthLogTime_ < kHealthLogPeriodMs))
            return false;
        const bool firstWindow = lastHealthLogTime_ == 0;
        lastHealthLogTime_ = now;
        if (firstWindow)
            return false;
        health.presents = submissions_.observedPresents();
        health.associations = submissions_.observedAssociations();
        health.queued = queuedTimestamps_;
        health.published = publishedTimestamps_;
        health.suppressed = suppressedTimestamps_;
        health.regressed = regressedTimestamps_;
        health.payloadReceived = frameTypePayloadReceived_;
        health.payloadValid = frameTypePayloadValid_;
        health.payloadCorrelated = frameTypeCorrelated_;
        health.payloadPending = correlation_.pendingPayloads().size();
        health.payloadPendingObserved = frameTypePendingObserved_;
        health.authoritative = frameTypeAuthoritative_;
        health.payloadDuplicate = frameTypePayloadDuplicate_;
        health.payloadLate = frameTypePayloadLate_;
        health.fallbackPublished = fallbackPublished_;
        health.fallbackSuppressed = fallbackSuppressed_;
        health.nvReceived = nvidiaAnnouncementsReceived_;
        health.nvUndecodable = nvidiaAnnouncementsUndecodable_;
        health.nvApplied = nvidiaAnnouncementsApplied_;
        health.nvFieldOffset = nvidiaAnnouncements_.located() ? static_cast<int32_t>(nvidiaAnnouncements_.offset())
                                                              : -1;
        health.nvFieldAbandoned = nvidiaAnnouncements_.abandoned();
        health.nvMaxDelayUs = DisplayTimingQpcToUs(nvidiaAnnouncedDelayMax_, qpcFrequency_);
        if (health.nvApplied != 0) {
            health.nvAverageDelayUs = DisplayTimingQpcToUs(
                nvidiaAnnouncedDelayTotal_ / static_cast<int64_t>(health.nvApplied), qpcFrequency_);
        }
        health.completions = completionsBySource_;
        health.blankAdjusted = blankAdjustedTimestamps_;
        health.blankUnresolved = blankUnresolvedTimestamps_;
        const uint32_t blankSource = verticalBlanks_.busiestSource();
        health.blankIntervalUs = DisplayTimingQpcToUs(verticalBlanks_.PeriodUs(blankSource), qpcFrequency_);
        health.blanksObserved = verticalBlanks_.observedBlanks(blankSource);
        SnapshotIntervals(health);
        return true;
    }

    // The busiest output is the one the overlay is reading; averaging several
    // would hide exactly the shape these statistics exist to expose.
    void SnapshotIntervals(DisplayTimingHealth& health) {
        const DisplayIntervalStats* busiest = nullptr;
        for (auto& output : lastPublishedByOutput_) {
            if (!busiest || output.second.intervals.count() > busiest->count())
                busiest = &output.second.intervals;
        }
        if (busiest)
            SetPublishedIntervals(health, *busiest);
        SetRuntimeIntervals(health, runtimeIntervals_);
        for (auto& output : lastPublishedByOutput_)
            output.second.intervals.StartWindow();
        runtimeIntervals_.StartWindow();
    }

    void LogHealthIfDue() {
        DisplayTimingHealth health;
        if (SnapshotHealth(health))
            LogDisplayTimingHealth(health);
    }

    void FlushLoop() {
        while (WaitForSingleObject(stopEvent_, kTraceFlushPeriodMs) == WAIT_TIMEOUT) {
            auto flushProperties = MakeProperties(sessionName_);
            FlushTraceW(session_, sessionName_, flushProperties.Get());
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            DrainReady(now.QuadPart, false);
            LogHealthIfDue();
        }
    }

    void StopTraceSession() {
        if (session_ != 0) {
            auto stopProperties = MakeProperties(sessionName_);
            ControlTraceW(session_, sessionName_, stopProperties.Get(), EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
        }
    }

    void StopNoexcept() noexcept {
        if (stopEvent_)
            SetEvent(stopEvent_);
        if (flushThread_.joinable())
            flushThread_.join();
        StopTraceSession();
        if (processThread_.joinable())
            processThread_.join();
        if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(traceHandle_);
            traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
        }
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        DrainReadyNoexcept(now.QuadPart);
        if (stopEvent_) {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }
        correlation_.Clear();
        nvidiaFlips_.Clear();
        nvidiaAnnouncements_.Reset();
        verticalBlanks_.Clear();
        submissions_.Clear();
    }

    std::atomic<bool> started_{false};
    std::atomic<DisplayTimingStatus> startupStatus_{DisplayTimingStatus::Unavailable};
    TRACEHANDLE session_ = 0;
    TRACEHANDLE traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    HANDLE stopEvent_ = nullptr;
    wchar_t sessionName_[kTraceSessionNameCapacity] = {};
    int64_t qpcFrequency_ = 0;
    int64_t lastPruneQpc_ = 0;
    std::thread processThread_;
    std::thread flushThread_;

    struct PublishedOutputState {
        int64_t lastTimestamp = 0;
        DisplayIntervalStats intervals;
    };

    std::mutex mutex_;
    std::vector<DisplayTimingTarget> targets_;
    DisplaySubmissionTracker submissions_;
    DisplayTimingCorrelation correlation_;
    NvidiaFlipAnnouncementDecoder nvidiaAnnouncements_;
    NvidiaFlipDelayTracker nvidiaFlips_;
    VerticalBlankClock verticalBlanks_;
    std::vector<PendingTimestamp> pendingTimestamps_;
    std::unordered_map<SharedDisplayTiming*, PublishedOutputState> lastPublishedByOutput_;
    DisplayIntervalStats runtimeIntervals_;
    uint32_t runtimeIntervalPid_ = 0;
    uint64_t blankAdjustedTimestamps_ = 0;
    uint64_t blankUnresolvedTimestamps_ = 0;
    uint64_t nextTimestampOrder_ = 1;
    ULONG observedTraceEventsLost_ = 0;
    ULONG loggedTraceEventsLost_ = 0;
    uint64_t lastTraceLossLogTime_ = 0;
    uint64_t lastHealthLogTime_ = 0;
    uint64_t queuedTimestamps_ = 0;
    uint64_t publishedTimestamps_ = 0;
    uint64_t suppressedTimestamps_ = 0;
    uint64_t regressedTimestamps_ = 0;
    uint64_t frameTypePayloadReceived_ = 0;
    uint64_t frameTypePayloadValid_ = 0;
    uint64_t frameTypeCorrelated_ = 0;
    uint64_t frameTypeAuthoritative_ = 0;
    uint64_t frameTypePendingObserved_ = 0;
    uint64_t frameTypePayloadDuplicate_ = 0;
    uint64_t frameTypePayloadLate_ = 0;
    uint64_t fallbackPublished_ = 0;
    uint64_t fallbackSuppressed_ = 0;
    uint64_t nvidiaAnnouncementsReceived_ = 0;
    uint64_t nvidiaAnnouncementsUndecodable_ = 0;
    uint64_t nvidiaAnnouncementsApplied_ = 0;
    int64_t nvidiaAnnouncedDelayTotal_ = 0;
    int64_t nvidiaAnnouncedDelayMax_ = 0;
    std::array<uint64_t, static_cast<std::size_t>(DisplayCompletionSource::Count)> completionsBySource_ = {};
};

DisplayTimingService::DisplayTimingService() : impl_(std::make_unique<Impl>()) {}

DisplayTimingService::~DisplayTimingService() = default;

void DisplayTimingService::Start() {
    impl_->Start();
}

void DisplayTimingService::UpdateTargets(const std::vector<DisplayTimingTarget>& targets) {
    impl_->UpdateTargets(targets);
}
