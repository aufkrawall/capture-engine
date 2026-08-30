#include "display_timing_service.h"
#include "display_timing_correlation.h"
#include "display_timing_policy.h"

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
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../common/display_timing_shared.h"
#include "../common/logging.h"

namespace {
constexpr GUID kRuntimeProvider = {0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}};
constexpr GUID kGraphicsKernelProvider = {
    0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};
constexpr GUID kFrameTypeProvider = {
    0xecaa4712, 0x4644, 0x442f, {0xb9, 0x4c, 0xa3, 0x2f, 0x6c, 0xf8, 0xa4, 0x99}};

constexpr uint16_t kRuntimePresentStart = 0x2a;
constexpr uint16_t kRuntimeMpoPresentStart = 0x37;
constexpr uint16_t kQueuePacketStart = 0xb2;
constexpr uint16_t kMmioFlip = 0x74;
constexpr uint16_t kMmioMpoFlip = 0x103;
constexpr uint16_t kVsync = 0x11;
constexpr uint16_t kVsyncMpo = 0x111;
constexpr uint16_t kHsyncMpo = 0x17e;
constexpr uint16_t kMpoPresentIds = 0x182;
constexpr uint16_t kGeneratedFlip = 0x2;

constexpr int64_t kTimestampReorderWindowUs = 24'000;
constexpr std::size_t kMaxPendingPresentsPerProcess = 16;
constexpr uint64_t kHealthLogPeriodMs = 10'000;
constexpr DWORD kTraceFlushPeriodMs = 8;
constexpr std::size_t kTraceSessionNameCapacity = 64;
constexpr ULONG kEventIdFilterType = 0x80000200;
constexpr ULONG kIgnoreZeroKeywordEvents = 0x00000010;

struct EventIdFilter {
    BOOLEAN filterIn;
    UCHAR reserved;
    USHORT count;
    USHORT events[1];
};

struct alignas(EVENT_TRACE_PROPERTIES) TracePropertiesBuffer {
    std::array<std::byte, sizeof(EVENT_TRACE_PROPERTIES) + kTraceSessionNameCapacity * sizeof(wchar_t)> storage{};

    EVENT_TRACE_PROPERTIES* Get() noexcept {
        return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    }
};

template <typename T>
bool ReadProperty(const EVENT_RECORD* event, const wchar_t* name, T& value,
                  ULONG arrayIndex = std::numeric_limits<ULONG>::max()) {
    PROPERTY_DATA_DESCRIPTOR descriptor = {};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
    descriptor.ArrayIndex = arrayIndex;
    ULONG size = sizeof(value);
    return TdhGetProperty(const_cast<EVENT_RECORD*>(event), 0, nullptr, 1, &descriptor, size,
                          reinterpret_cast<PBYTE>(&value)) == ERROR_SUCCESS;
}

TracePropertiesBuffer MakeProperties(const wchar_t* name) noexcept {
    const std::size_t nameBytes = (std::wcslen(name) + 1) * sizeof(wchar_t);
    TracePropertiesBuffer buffer;
    auto* properties = buffer.Get();
    properties->Wnode.BufferSize = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 16;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
    properties->FlushTimer = 1;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    std::memcpy(buffer.storage.data() + properties->LoggerNameOffset, name, nameBytes);
    return buffer;
}

ULONG EnableFilteredProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords,
                             std::initializer_list<USHORT> eventIds) {
    const std::size_t filterSize = offsetof(EventIdFilter, events) + eventIds.size() * sizeof(USHORT);
    std::vector<std::byte> filterStorage(filterSize);
    auto* filter = reinterpret_cast<EventIdFilter*>(filterStorage.data());
    filter->filterIn = TRUE;
    filter->reserved = 0;
    filter->count = static_cast<USHORT>(eventIds.size());
    std::copy(eventIds.begin(), eventIds.end(), filter->events);

    EVENT_FILTER_DESCRIPTOR descriptor = {};
    descriptor.Ptr = reinterpret_cast<ULONGLONG>(filter);
    descriptor.Size = static_cast<ULONG>(filterStorage.size());
    descriptor.Type = kEventIdFilterType;

    ENABLE_TRACE_PARAMETERS parameters = {};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableProperty = kIgnoreZeroKeywordEvents;
    parameters.EnableFilterDesc = &descriptor;
    parameters.FilterDescCount = 1;
    return EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, keywords, 0, 0,
                          &parameters);
}

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
        swprintf(sessionName_, std::size(sessionName_), L"CE_DisplayTiming_%08X", GetCurrentProcessId());
        auto properties = MakeProperties(sessionName_);

        ULONG status = StartTraceW(&session_, sessionName_, properties.Get());
        if (status != ERROR_SUCCESS) {
            SetStartupFailure(status);
            return;
        }

        status = EnableFilteredProvider(session_, kRuntimeProvider, 0x8000000000000002ull,
                                        {kRuntimePresentStart, kRuntimeMpoPresentStart});
        if (status == ERROR_SUCCESS) {
            status = EnableFilteredProvider(session_, kGraphicsKernelProvider, 0x1,
                                            {kQueuePacketStart, kMmioFlip, kMmioMpoFlip, kVsync, kVsyncMpo,
                                             kHsyncMpo, kMpoPresentIds});
        }
        if (status == ERROR_SUCCESS) {
            const ULONG optionalStatus =
                EnableFilteredProvider(session_, kFrameTypeProvider, 0x1, {kGeneratedFlip});
            if (optionalStatus != ERROR_SUCCESS) {
                LogWarn("[DisplayTiming] Generated-frame timestamp events are unavailable: %lu", optionalStatus);
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
                lastPublishedByOutput_.try_emplace(target.output, 0);
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
                auto& pending = pendingRuntimePresents_[header.ProcessId];
                // A present that never reached a kernel submission is dropped
                // by age below; this only bounds a runaway producer.
                if (pending.size() >= kMaxPendingPresentsPerProcess)
                    pending.pop_front();
                pending.push_back({header.ThreadId, header.TimeStamp.QuadPart});
                ++observedRuntimePresents_;
            }
            return;
        }

        if (IsEqualGUID(header.ProviderId, kGraphicsKernelProvider)) {
            HandleGraphicsKernelEvent(event);
            return;
        }

        if (IsEqualGUID(header.ProviderId, kFrameTypeProvider) && header.EventDescriptor.Id == kGeneratedFlip)
            HandleGeneratedFlip(event);
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
        // The submitting thread belongs to the presenting process even when it
        // is not the thread that called Present, so the process is the key and
        // the thread only refines the choice within it.
        const auto process = pendingRuntimePresents_.find(event->EventHeader.ProcessId);
        if (process == pendingRuntimePresents_.end() || process->second.empty())
            return;
        auto& pending = process->second;
        std::array<uint32_t, kMaxPendingPresentsPerProcess> pendingThreadIds = {};
        const std::size_t pendingCount = std::min(pending.size(), pendingThreadIds.size());
        for (std::size_t i = 0; i < pendingCount; ++i)
            pendingThreadIds[i] = pending[i].threadId;
        const std::size_t selected =
            SelectDisplaySubmissionPresent(pendingThreadIds.data(), pendingCount, event->EventHeader.ThreadId);
        if (selected == kNoPendingDisplayPresent)
            return;
        submitAssociations_[submitSequence].push_back(
            {event->EventHeader.ProcessId, event->EventHeader.TimeStamp.QuadPart, nextAssociationId_++});
        pending.erase(pending.begin() + static_cast<std::deque<PendingRuntimePresent>::difference_type>(selected));
        if (pending.empty())
            pendingRuntimePresents_.erase(process);
        ++observedSubmitAssociations_;
    }

    void HandleVsync(EVENT_RECORD* event) {
        uint64_t fenceId = 0;
        if (!ReadProperty(event, L"FlipFenceId", fenceId) || fenceId == 0)
            return;
        PublishForSubmit(static_cast<uint32_t>(fenceId >> 32u), event->EventHeader.TimeStamp.QuadPart,
                         DisplayCompletionKind::Sync, true);
    }

    void HandleMpoSync(EVENT_RECORD* event) {
        uint32_t count = 0;
        if (!ReadProperty(event, L"FlipEntryCount", count) || count == 0 || count > 64)
            return;
        std::array<uint32_t, 64> publishedPids = {};
        std::size_t publishedCount = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t encodedSequence = 0;
            if (!ReadProperty(event, L"FlipSubmitSequence", encodedSequence, i) || encodedSequence == 0)
                continue;
            const uint32_t submitSequence = static_cast<uint32_t>(encodedSequence >> 32u);
            const SubmitAssociation* association = FindSubmitAssociation(submitSequence);
            if (!association)
                continue;
            const uint32_t processId = association->processId;
            if (std::find(publishedPids.begin(), publishedPids.begin() + publishedCount, processId) ==
                publishedPids.begin() + publishedCount) {
                QueueTimestamp(processId, association->associationId, event->EventHeader.TimeStamp.QuadPart,
                               DisplayCompletionKind::Sync);
                publishedPids[publishedCount++] = processId;
            }
            EraseSubmitAssociation(submitSequence);
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
        const SubmitAssociation* association = FindSubmitAssociation(submitSequence);
        if (!association)
            return;
        for (uint32_t i = 0; i < planeCount; ++i) {
            uint64_t presentId = 0;
            uint32_t layer = 0;
            if (ReadProperty(event, L"PresentId", presentId, i) && ReadProperty(event, L"LayerIndex", layer, i)) {
                const DisplayLayerPresentKey layerKey = {displaySource, layer, presentId};
                correlation_.Associate(layerKey, {association->processId, association->associationId,
                                                  event->EventHeader.TimeStamp.QuadPart});
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
                             DisplayCompletionKind::Immediate, true);
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
        if (status != 5 && status != 15)
            PublishForSubmit(static_cast<uint32_t>(encodedSequence >> 32u), event->EventHeader.TimeStamp.QuadPart,
                             DisplayCompletionKind::Immediate, true);
    }

    void PublishForSubmit(uint32_t submitSequence, int64_t timestamp, DisplayCompletionKind completionKind,
                          bool erase) {
        const SubmitAssociation* association = FindSubmitAssociation(submitSequence);
        if (!association)
            return;
        QueueTimestamp(association->processId, association->associationId, timestamp, completionKind);
        if (erase)
            EraseSubmitAssociation(submitSequence);
    }

    const SubmitAssociation* FindSubmitAssociation(uint32_t submitSequence) const {
        const auto association = submitAssociations_.find(submitSequence);
        if (association != submitAssociations_.end() && !association->second.empty())
            return &association->second.front();
        return nullptr;
    }

    void EraseSubmitAssociation(uint32_t submitSequence) {
        const auto association = submitAssociations_.find(submitSequence);
        if (association == submitAssociations_.end())
            return;
        association->second.pop_front();
        if (association->second.empty())
            submitAssociations_.erase(association);
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
                        DisplayCompletionKind completionKind) {
        if (timestamp <= 0)
            return;
        if (completionKind != DisplayCompletionKind::Unconditional) {
            // The fallback itself owns the association tombstone.  This makes
            // a later FrameType telemetry-only even when no payload preceded
            // the fallback (the 24 ms watermark is a bounded policy, not a
            // causal/no-late-events guarantee).
            correlation_.QueueFallback(processId, associationId, timestamp, completionKind,
                                       pendingTimestamps_, nextTimestampOrder_);
            ++queuedTimestamps_;
            return;
        }
        pendingTimestamps_.push_back({processId, associationId, timestamp, completionKind, nextTimestampOrder_++});
        ++queuedTimestamps_;
    }

    bool ShouldPublish(const PendingTimestamp& pending) const {
        return correlation_.ShouldPublish(pending);
    }

    void DrainReady(int64_t nowQpc, bool force) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lastPruneQpc_ == 0 || nowQpc - lastPruneQpc_ >= qpcFrequency_ * 5) {
            PruneAssociations(nowQpc - qpcFrequency_ * 10);
            lastPruneQpc_ = nowQpc;
        }
        if (pendingTimestamps_.empty())
            return;
        std::sort(pendingTimestamps_.begin(), pendingTimestamps_.end(), [](const auto& a, const auto& b) {
            return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.arrivalOrder < b.arrivalOrder;
        });
        const int64_t cutoff = nowQpc - (kTimestampReorderWindowUs * qpcFrequency_) / 1'000'000;
        const int64_t publishUs = DisplayTimingQpcToUs(nowQpc, qpcFrequency_);
        std::size_t consumed = 0;
        for (auto& pending : pendingTimestamps_) {
            if (!force && pending.timestamp > cutoff)
                break;
            if (ShouldPublish(pending)) {
                PublishTimestamp(pending.processId, pending.timestamp, publishUs);
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
        std::sort(pendingTimestamps_.begin(), pendingTimestamps_.end(), [](const auto& a, const auto& b) {
            return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.arrivalOrder < b.arrivalOrder;
        });
        const int64_t publishUs = DisplayTimingQpcToUs(nowQpc, qpcFrequency_);
        for (const auto& pending : pendingTimestamps_)
            if (ShouldPublish(pending))
                PublishTimestamp(pending.processId, pending.timestamp, publishUs);
        pendingTimestamps_.clear();
    }

    void PruneAssociations(int64_t cutoff) {
        for (auto it = pendingRuntimePresents_.begin(); it != pendingRuntimePresents_.end();) {
            auto& presents = it->second;
            while (!presents.empty() && presents.front().timestamp < cutoff)
                presents.pop_front();
            it = presents.empty() ? pendingRuntimePresents_.erase(it) : std::next(it);
        }
        for (auto mapIt = submitAssociations_.begin(); mapIt != submitAssociations_.end();) {
            auto& associations = mapIt->second;
            while (!associations.empty() && associations.front().timestamp < cutoff)
                associations.pop_front();
            mapIt = associations.empty() ? submitAssociations_.erase(mapIt) : std::next(mapIt);
        }
        correlation_.Prune(cutoff);
    }

    void PublishTimestamp(uint32_t processId, int64_t timestamp, int64_t publishUs) {
        for (const auto& target : targets_) {
            if (!target.output)
                continue;
            if (target.sourcePid != processId && target.rendererPid != processId)
                continue;
            const auto lastPublished = lastPublishedByOutput_.find(target.output);
            if (lastPublished == lastPublishedByOutput_.end())
                continue;
            if (timestamp <= lastPublished->second) {
                target.output->droppedTimestampCount.fetch_add(1, std::memory_order_relaxed);
                ++regressedTimestamps_;
                continue;
            }
            lastPublished->second = timestamp;
            target.output->Publish(DisplayTimingQpcToUs(timestamp, qpcFrequency_), publishUs);
            ++publishedTimestamps_;
        }
    }

    void LogHealthIfDue() {
        uint64_t presents = 0, associations = 0, queued = 0, published = 0, suppressed = 0, regressed = 0;
        uint64_t payloadReceived = 0, payloadValid = 0, payloadCorrelated = 0, payloadPending = 0,
                 payloadPendingObserved = 0, authoritative = 0, fallbackPublished = 0, fallbackSuppressed = 0,
                 payloadDuplicate = 0,
                 payloadLate = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t now = GetTickCount64();
            if (targets_.empty() || (lastHealthLogTime_ != 0 && now - lastHealthLogTime_ < kHealthLogPeriodMs))
                return;
            const bool firstWindow = lastHealthLogTime_ == 0;
            lastHealthLogTime_ = now;
            if (firstWindow)
                return;
            presents = observedRuntimePresents_;
            associations = observedSubmitAssociations_;
            queued = queuedTimestamps_;
            published = publishedTimestamps_;
            suppressed = suppressedTimestamps_;
            regressed = regressedTimestamps_;
            payloadReceived = frameTypePayloadReceived_;
            payloadValid = frameTypePayloadValid_;
            payloadCorrelated = frameTypeCorrelated_;
            payloadPending = correlation_.pendingPayloads().size();
            authoritative = frameTypeAuthoritative_;
            fallbackPublished = fallbackPublished_;
            fallbackSuppressed = fallbackSuppressed_;
            payloadPendingObserved = frameTypePendingObserved_;
            payloadDuplicate = frameTypePayloadDuplicate_;
            payloadLate = frameTypePayloadLate_;
        }
        if (published == 0) {
            LogWarn(
                "[DisplayTiming] No screen-change timestamp published yet: runtimePresents=%llu submitAssociations"
                "=%llu queued=%llu suppressed=%llu regressed=%llu frameType(received=%llu valid=%llu "
                "matched=%llu pendingCurrent=%llu pendingObserved=%llu authoritativeQueued=%llu duplicate=%llu late=%llu) "
                "fallback(committed=%llu suppressed=%llu)",
                presents, associations, queued, suppressed, regressed, payloadReceived, payloadValid,
                payloadCorrelated, payloadPending, payloadPendingObserved, authoritative, payloadDuplicate, payloadLate,
                fallbackPublished,
                fallbackSuppressed);
        } else if (Log_IsEnabled(LogLevel::Debug)) {
            LogDebug(
                "[DisplayTiming] runtimePresents=%llu submitAssociations=%llu queued=%llu published=%llu "
                "suppressed=%llu regressed=%llu frameType(received=%llu valid=%llu matched=%llu pendingCurrent=%llu "
                "pendingObserved=%llu authoritativeQueued=%llu duplicate=%llu late=%llu) "
                "fallback(committed=%llu suppressed=%llu)",
                presents, associations, queued, published, suppressed, regressed, payloadReceived, payloadValid,
                payloadCorrelated, payloadPending, payloadPendingObserved, authoritative, payloadDuplicate, payloadLate,
                fallbackPublished, fallbackSuppressed);
        }
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

    std::mutex mutex_;
    std::vector<DisplayTimingTarget> targets_;
    std::unordered_map<uint32_t, std::deque<PendingRuntimePresent>> pendingRuntimePresents_;
    std::unordered_map<uint32_t, std::deque<SubmitAssociation>> submitAssociations_;
    DisplayTimingCorrelation correlation_;
    std::vector<PendingTimestamp> pendingTimestamps_;
    std::unordered_map<SharedDisplayTiming*, int64_t> lastPublishedByOutput_;
    uint64_t nextAssociationId_ = 1;
    uint64_t nextTimestampOrder_ = 1;
    ULONG observedTraceEventsLost_ = 0;
    ULONG loggedTraceEventsLost_ = 0;
    uint64_t lastTraceLossLogTime_ = 0;
    uint64_t lastHealthLogTime_ = 0;
    uint64_t observedRuntimePresents_ = 0;
    uint64_t observedSubmitAssociations_ = 0;
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
};

DisplayTimingService::DisplayTimingService() : impl_(std::make_unique<Impl>()) {}

DisplayTimingService::~DisplayTimingService() = default;

void DisplayTimingService::Start() {
    impl_->Start();
}

void DisplayTimingService::UpdateTargets(const std::vector<DisplayTimingTarget>& targets) {
    impl_->UpdateTargets(targets);
}
