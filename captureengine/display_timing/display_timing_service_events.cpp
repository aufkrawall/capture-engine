#include "display_timing_service_internal.h"

using namespace display_timing_etw;

// The ETW event reducers: runtime presents and kernel submissions in, flip
// completions matched back to them, and the timestamps queued for publication.

void WINAPI DisplayTimingService::Impl::EventRecordThunk(EVENT_RECORD* event) {
    static_cast<Impl*>(event->UserContext)->HandleEvent(event);
}

bool DisplayTimingService::Impl::IsTrackedProcess(uint32_t processId) const {
    return std::any_of(targets_.begin(), targets_.end(), [&](const DisplayTimingTarget& target) {
        return target.sourcePid == processId || (target.rendererPid != 0 && target.rendererPid == processId);
    });
}

void DisplayTimingService::Impl::HandleEvent(EVENT_RECORD* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& header = event->EventHeader;
    if (IsEqualGUID(header.ProviderId, kRuntimeProvider)) {
        if ((header.EventDescriptor.Id == kRuntimePresentStart ||
             header.EventDescriptor.Id == kRuntimeMpoPresentStart) &&
            IsTrackedProcess(header.ProcessId)) {
            // SyncInterval >= 1 is what makes a flip unable to tear; the
            // refresh bound applies to nothing else.
            uint32_t syncInterval = 0;
            const int32_t presentSync = ReadProperty(event, L"SyncInterval", syncInterval)
                                            ? static_cast<int32_t>(std::min<uint32_t>(syncInterval, 4))
                                            : kUnknownSyncInterval;
            submissions_.ObserveRuntimePresent(header.ProcessId, header.ThreadId,
                                               header.TimeStamp.QuadPart, presentSync);
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

void DisplayTimingService::Impl::HandleNvidiaFlipRequest(EVENT_RECORD* event) {
    nvidiaSchedule_.ObserveRequest(event->EventHeader.ThreadId, event->UserData, event->UserDataLength,
                                   event->EventHeader.TimeStamp.QuadPart);
}

void DisplayTimingService::Impl::HandleGraphicsKernelEvent(EVENT_RECORD* event) {
    const auto& header = event->EventHeader;
    switch (header.EventDescriptor.Id) {
        case kQueuePacketStart:
            HandleQueuePacket(event);
            break;
        case kQueuePacketStop:
            HandleQueuePacketStop(event);
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

void DisplayTimingService::Impl::HandleQueuePacket(EVENT_RECORD* event) {
    uint32_t submitSequence = 0;
    uint32_t isPresent = 0;
    if (!ReadProperty(event, L"SubmitSequence", submitSequence) ||
        !ReadProperty(event, L"bPresent", isPresent) || isPresent == 0) {
        return;
    }
    // The compositor is followed only while it carries a composed process.
    const bool compositor = composed_.IsCompositor(event->EventHeader.ProcessId);
    if (!compositor && !IsTrackedProcess(event->EventHeader.ProcessId))
        return;
    bool isFallback = false;
    if (submissions_.Associate(event->EventHeader.ProcessId, event->EventHeader.ThreadId, submitSequence,
                               event->EventHeader.TimeStamp.QuadPart, &isFallback)) {
        if (isFallback && !compositor) {
            if (runtimeIntervalPid_ != event->EventHeader.ProcessId) {
                runtimeIntervalPid_ = event->EventHeader.ProcessId;
                runtimeIntervals_.StartWindow();
            }
            runtimeIntervals_.Observe(
                DisplayTimingQpcToUs(event->EventHeader.TimeStamp.QuadPart, qpcFrequency_));
        }
    }
}

void DisplayTimingService::Impl::HandleQueuePacketStop(EVENT_RECORD* event) {
    uint32_t submitSequence = 0;
    if (!composed_.active() || !ReadProperty(event, L"SubmitSequence", submitSequence))
        return;
    if (const SubmitAssociation* association = submissions_.Find(submitSequence)) {
        composed_.ObserveReady(submitSequence, *association, event->EventHeader.TimeStamp.QuadPart,
                               kMaxSubmitToCompletionUs * qpcFrequency_ / 1'000'000);
    }
}

void DisplayTimingService::Impl::PublishComposedFrame(int64_t compositorSubmitTimestamp, int64_t timestamp,
                                                      uint32_t displaySource) {
    const std::optional<ComposedFrame> frame = composed_.TakeFrameShownBy(compositorSubmitTimestamp);
    if (!frame || !submissions_.EraseAssociation(frame->submitSequence, frame->associationId))
        return;
    QueueTimestamp(frame->processId, frame->associationId, timestamp, DisplayCompletionKind::Sync,
                   frame->presentStartTimestamp, displaySource);
}

void DisplayTimingService::Impl::HandleVsync(EVENT_RECORD* event) {
    uint32_t displaySource = 0;
    if (ReadProperty(event, L"VidPnSourceId", displaySource)) {
        verticalBlanks_.Observe(displaySource, event->EventHeader.TimeStamp.QuadPart);
        if (displaySource == verticalBlanks_.busiestSource())
            blankIntervals_.Observe(DisplayTimingQpcToUs(event->EventHeader.TimeStamp.QuadPart, qpcFrequency_));
    }
    uint64_t fenceId = 0;
    if (!ReadProperty(event, L"FlipFenceId", fenceId) || fenceId == 0)
        return;
    PublishForSubmit(static_cast<uint32_t>(fenceId >> 32u), event->EventHeader.TimeStamp.QuadPart,
                     DisplayCompletionKind::Sync, true, DisplayCompletionSource::VSyncDpc, displaySource);
}

void DisplayTimingService::Impl::HandleMpoSync(EVENT_RECORD* event) {
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
        const SubmitAssociation* association =
            submissions_.FindForCompletion(submitSequence, event->EventHeader.TimeStamp.QuadPart);
        if (!association)
            continue;
        const uint32_t processId = association->processId;
        if (composed_.IsCompositor(processId)) {
            PublishComposedFrame(association->timestamp, event->EventHeader.TimeStamp.QuadPart, displaySource);
            submissions_.Erase(submitSequence);
            continue;
        }
        ++ownCompletions_;
        if (std::find(publishedPids.begin(), publishedPids.begin() + publishedCount, processId) ==
            publishedPids.begin() + publishedCount) {
            QueueTimestamp(processId, association->associationId, event->EventHeader.TimeStamp.QuadPart,
                           DisplayCompletionKind::Sync, association->presentStartTimestamp, displaySource,
                           association->syncInterval >= 1);
            ++completionsBySource_[static_cast<std::size_t>(source)];
            latchIntervals_.Observe(
                DisplayTimingQpcToUs(event->EventHeader.TimeStamp.QuadPart, qpcFrequency_));
            publishedPids[publishedCount++] = processId;
        }
        submissions_.Erase(submitSequence);
    }
}

void DisplayTimingService::Impl::HandleMpoPresentIds(EVENT_RECORD* event) {
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
    const SubmitAssociation* association =
        submissions_.FindForCompletion(submitSequence, event->EventHeader.TimeStamp.QuadPart);
    if (!association || composed_.IsCompositor(association->processId))
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

void DisplayTimingService::Impl::HandleGeneratedFlip(EVENT_RECORD* event) {
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

void DisplayTimingService::Impl::HandleImmediateFlip(EVENT_RECORD* event) {
    uint32_t submitSequence = 0;
    uint32_t flags = 0;
    if (ReadProperty(event, L"FlipSubmitSequence", submitSequence) && ReadProperty(event, L"Flags", flags) &&
        (flags & 2u) != 0) {
        PublishForSubmit(submitSequence, event->EventHeader.TimeStamp.QuadPart,
                         DisplayCompletionKind::Immediate, true, DisplayCompletionSource::ImmediateFlip);
    }
}

void DisplayTimingService::Impl::HandleImmediateMpoFlip(EVENT_RECORD* event) {
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
        nvidiaSchedule_.TakeFlipDelay(event->EventHeader.ThreadId, false);
        return;
    }
    // Consume the announcement on every immediate flip, not only on the ones
    // that resolve to a tracked process: an announcement left behind by a
    // flip we do not publish would otherwise be applied to an unrelated
    // later flip on the same driver thread.
    const NvidiaFlipDelay announced = nvidiaSchedule_.TakeFlipDelay(event->EventHeader.ThreadId);
    PublishForSubmit(static_cast<uint32_t>(encodedSequence >> 32u),
                     event->EventHeader.TimeStamp.QuadPart + announced.delay,
                     DisplayCompletionKind::Immediate, true, DisplayCompletionSource::ImmediateMultiPlaneFlip);
}

void DisplayTimingService::Impl::PublishForSubmit(uint32_t submitSequence, int64_t timestamp,
                                                  DisplayCompletionKind completionKind, bool erase,
                                                  DisplayCompletionSource source, uint32_t displaySource) {
    const SubmitAssociation* association = submissions_.FindForCompletion(submitSequence, timestamp);
    if (!association)
        return;
    if (composed_.IsCompositor(association->processId)) {
        PublishComposedFrame(association->timestamp, timestamp, displaySource);
        if (erase)
            submissions_.Erase(submitSequence);
        return;
    }
    ++ownCompletions_;
    QueueTimestamp(association->processId, association->associationId, timestamp, completionKind,
                   association->presentStartTimestamp, displaySource,
                   completionKind == DisplayCompletionKind::Sync && association->syncInterval >= 1);
    ++completionsBySource_[static_cast<std::size_t>(source)];
    if (erase)
        submissions_.Erase(submitSequence);
}

void DisplayTimingService::Impl::ConsumeCorrelationPayloads() {
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

void DisplayTimingService::Impl::QueueTimestamp(uint32_t processId, uint64_t associationId, int64_t timestamp,
                                                DisplayCompletionKind completionKind, int64_t presentStartTimestamp,
                                                uint32_t displaySource, bool synchronizedFlip) {
    if (timestamp <= 0)
        return;
    if (completionKind != DisplayCompletionKind::Unconditional) {
        // The fallback itself owns the association tombstone.  This makes
        // a later FrameType telemetry-only even when no payload preceded
        // the fallback (the 24 ms watermark is a bounded policy, not a
        // causal/no-late-events guarantee).
        correlation_.QueueFallback(processId, associationId, timestamp, completionKind,
                                   pendingTimestamps_, nextTimestampOrder_, presentStartTimestamp,
                                   displaySource, synchronizedFlip);
        ++queuedTimestamps_;
        return;
    }
    pendingTimestamps_.push_back({processId, associationId, timestamp, completionKind,
                                  nextTimestampOrder_++, presentStartTimestamp, displaySource});
    ++queuedTimestamps_;
}

bool DisplayTimingService::Impl::ShouldPublish(const PendingTimestamp& pending) const {
    return correlation_.ShouldPublish(pending);
}
