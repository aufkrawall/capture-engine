#pragma once

#include <intrin.h>  // for _mm_pause
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>  // for memcpy in seqlock helpers
#include <type_traits>

#include "../build_identity.h"

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#include "abi_constants_and_config.h"

// CaptureState: the seqlock-published live capture/recording status block.

#pragma pack(push, 8)

struct alignas(8) CaptureState {
    std::atomic<int64_t> recordingStartTime{0};  // File-output / REC-indicator start time
    std::atomic<double> currentFPS{0.0};         // Atomic to prevent torn reads
    std::atomic<double> gameFPS{0.0};            // Atomic to prevent torn reads
    std::atomic<uint32_t> hostDroppedFrames{0};  // Atomic counter

    // Additional smoothness indicators (watertight tracking)
    std::atomic<uint32_t> duplicateFrames{0};  // Same frame re-encoded (no new frame available)
    std::atomic<uint32_t> lateFrames{0};       // Encode time exceeded frame budget

    std::atomic<uint32_t> encoderOverloadFlags{0};
    std::atomic<uint32_t> encoderSustainFpsX100{0};
    std::atomic<uint32_t> muxQueueBytes{0};
    std::atomic<uint32_t> muxQueuePackets{0};
    std::atomic<uint32_t> muxQueuePeakBytes{0};
    std::atomic<uint32_t> muxQueuePeakPackets{0};
    std::atomic<uint32_t> muxBackpressureCount{0};
    std::atomic<uint32_t> muxBackpressureWaitUs{0};
    std::atomic<uint32_t> muxBackpressureMaxWaitUs{0};

    std::atomic<uint32_t> capturePhase{static_cast<uint32_t>(CapturePipelinePhase::kIdle)};
    std::atomic<uint32_t> sourceFramesReceived{0};
    std::atomic<uint32_t> framesQueued{0};
    std::atomic<uint32_t> framesEncoded{0};
    std::atomic<uint32_t> liveFramesEncoded{0};
    std::atomic<uint32_t> drainFramesEncoded{0};
    std::atomic<uint32_t> invalidFrameMetadata{0};
    std::atomic<uint32_t> invalidSharedHandles{0};
    std::atomic<uint32_t> injectPacingDrops{0};
    std::atomic<uint32_t> injectCadenceDrops{0};
    std::atomic<uint32_t> injectTrimmedFrames{0};
    std::atomic<uint32_t> injectProducerCaptureLockDrops{0};
    std::atomic<uint32_t> injectProducerCpuLeaseBusyDrops{0};
    std::atomic<uint32_t> injectProducerGpuBusyDrops{0};
    std::atomic<uint32_t> injectProducerMetadataFullDrops{0};
    std::atomic<uint32_t> injectFrameReadySignals{0};
    std::atomic<uint32_t> injectPublicationToIngestAvgUs{0};
    std::atomic<uint32_t> injectPublicationToIngestMaxUs{0};
    std::atomic<uint32_t> encoderTimerWakeLateAvgUs{0};
    std::atomic<uint32_t> encoderTimerWakeLateMaxUs{0};
    std::atomic<uint32_t> deferredFrames{0};
    std::atomic<uint32_t> repeatedDeferredFrames{0};
    std::atomic<uint32_t> consecutiveDeferredFrames{0};
    std::atomic<uint32_t> maxConsecutiveDeferredFrames{0};
    std::atomic<uint32_t> duplicateFramesNoSource{0};
    std::atomic<uint32_t> duplicateFramesDeferred{0};
    std::atomic<uint32_t> duplicateFramesTimerRebase{0};
    std::atomic<uint32_t> duplicateFramesDrain{0};
    std::atomic<uint32_t> consecutiveDuplicateFrames{0};
    std::atomic<uint32_t> maxConsecutiveDuplicateFrames{0};
    std::atomic<uint32_t> frameIndexRegressions{0};
    std::atomic<uint32_t> textureReuseAnomalies{0};
    std::atomic<uint32_t> sourceTimestampRegressions{0};
    std::atomic<uint32_t> sourceTimestampStalls{0};
    std::atomic<uint32_t> timerRebases{0};
    std::atomic<uint32_t> bufferedInjectDepthPeak{0};
    std::atomic<uint32_t> encoderQueuePeakDepth{0};
    std::atomic<uint32_t> packetDurationClamps{0};
    std::atomic<uint32_t> negativePtsCount{0};
    std::atomic<uint32_t> nonMonotonicPtsCount{0};
    std::atomic<uint32_t> frameAgeAvgUs{0};
    std::atomic<uint32_t> frameAgeMaxUs{0};
    std::atomic<uint32_t> selectionErrorAvgUs{0};
    std::atomic<uint32_t> selectionErrorMaxUs{0};
    std::atomic<int32_t> selectionErrorSignedAvgUs{0};
    std::atomic<uint32_t> selectionEarlyMaxUs{0};
    std::atomic<uint32_t> selectionLateMaxUs{0};
    std::atomic<uint32_t> wgcSelectionErrorAvgUs{0};
    std::atomic<uint32_t> wgcSelectionErrorMaxUs{0};
    std::atomic<int32_t> wgcSelectionErrorSignedAvgUs{0};
    std::atomic<uint32_t> wgcSelectionEarlyMaxUs{0};
    std::atomic<uint32_t> wgcSelectionLateMaxUs{0};
    std::atomic<uint32_t> oldestBufferedFrameAgeUs{0};
    std::atomic<uint32_t> wgcSourceFrameIntervalAvgUs{0};
    std::atomic<uint32_t> wgcSourceFrameJitterAvgUs{0};
    std::atomic<uint32_t> wgcSourceFrameJitterMaxUs{0};
    std::atomic<uint32_t> wgcSourceToCopyLatencyAvgUs{0};
    std::atomic<uint32_t> wgcSourceToCopyLatencyMaxUs{0};
    std::atomic<uint32_t> wgcTargetFps{0};
    std::atomic<uint32_t> wgcDeliveredFramesPerSec{0};
    std::atomic<uint32_t> wgcDeliveredMin250Fps{0};
    std::atomic<uint32_t> wgcDeliveredMin500Fps{0};
    std::atomic<uint32_t> wgcInputMin250Fps{0};
    std::atomic<uint32_t> wgcInputMin500Fps{0};
    std::atomic<uint32_t> wgcAudioLeadExcessSamples{0};
    std::atomic<uint32_t> wgcQueueEmptyTickPermille{0};
    std::atomic<uint32_t> wgcBufferedAtTickAvgPermille{0};
    std::atomic<uint32_t> wgcBufferedAtTickMin{0};
    std::atomic<uint32_t> wgcStarvedTickCount{0};
    std::atomic<uint32_t> wgcSingleFrameTickCount{0};
    std::atomic<uint32_t> wgcCaptureHealthFlags{0};  // bit0=source-starved, bit1=scheduler-limited
    std::atomic<uint32_t> wgcCaptureHealthFps{0};    // recent WGC input min-250 FPS for source warnings
    std::atomic<uint32_t> encoderBottlenecked{0};    // 1 when encoder can't sustain target FPS
    // Recording-level capacity history. Unlike encoderOverloadFlags, the cause
    // and degradation bits remain latched until the next recording so recovery
    // cannot erase evidence that the immutable CFR timeline was damaged.
    std::atomic<uint32_t> recordingHealthFlags{0};
    std::atomic<uint32_t> recordingTimelineDebtMs{0};
    std::atomic<uint32_t> recordingPeakTimelineDebtMs{0};

    // Command flags (controller -> media process via shared memory)
    // Using std::atomic for proper cross-process visibility and memory ordering
    std::atomic<bool> cmdStartRecording{false};
    std::atomic<bool> cmdStopRecording{false};
    std::atomic<bool> ackRecordingStarted{false};
    std::atomic<bool> ackRecordingStopped{false};
    std::atomic<uint32_t> recordingFailureCode{static_cast<uint32_t>(RecordingFailureCode::None)};

    // Media -> sensor service. This is deliberately separate from sourcePid_,
    // which remains hook-owned and continues to drive injection/config behavior.
    // A seqlock prevents a PID/LUID pair from being observed across retargets.
    std::atomic<uint32_t> screenGrabTargetSequence{0};
    std::atomic<uint32_t> screenGrabTargetPid{0};
    std::atomic<int32_t> screenGrabAdapterLuidLow{0};
    std::atomic<int32_t> screenGrabAdapterLuidHigh{0};
    std::atomic<uint32_t> screenGrabTargetActive{0};

    // Screenshot request/result protocol (host -> hook -> host). screenshotPath is
    // the request-owned .part path; the hook atomically publishes the matching
    // .ready payload before completing and signaling screenshotCompletionEventName.
    std::atomic<uint64_t> screenshotRequestId{0};
    std::atomic<uint64_t> screenshotCompletedRequestId{0};
    std::atomic<uint32_t> screenshotStatus{static_cast<uint32_t>(ScreenshotRequestStatus::Idle)};
    std::atomic<uint32_t> screenshotError{0};
    std::atomic<uint32_t> screenshotPayloadKind{static_cast<uint32_t>(ScreenshotPayloadKind::None)};
    char screenshotPath[512]{};
    char screenshotCompletionEventName[128]{};

    std::atomic<bool> captureRequested{false};       // Hooks should keep feeding frames (warmup + live recording)
    std::atomic<bool> isRecording{false};            // File output and REC overlay indicator are live
    std::atomic<bool> audioOnly{false};              // true = audio-only recording mode (no video)
    std::atomic<bool> vulkanLayerActive{false};      // Set by Vulkan layer when initialized
    std::atomic<uint32_t> runtimeFlags{0};           // Cross-API coordination (overlay ownership, etc.)
    std::atomic<uint32_t> vulkanPresentThreadId{0};  // Thread ID currently presenting via Vulkan
    std::atomic<uint64_t> vulkanPresentTick{0};      // GetTickCount64 of last Vulkan present

    // Transient overlay notification (host -> hook overlay)
    // notificationExpiry: GetTickCount64() value after which notification disappears (0 = none)
    // notificationType: OverlayNotificationType
    std::atomic<uint64_t> notificationExpiry{0};
    std::atomic<uint32_t> notificationType{0};

    bool HasRuntimeFlag(uint32_t flag) const {
        return (runtimeFlags.load(std::memory_order_acquire) & flag) != 0;
    }

    bool IsInjectVideoCaptureRequested() const {
        return captureRequested.load(std::memory_order_acquire) &&
               HasRuntimeFlag(kCaptureRuntimeFlagInjectVideoCaptureRequested);
    }

    RecordingStartIntent GetRecordingStartIntent() const {
        const uint32_t flags = runtimeFlags.load(std::memory_order_acquire);
        if ((flags & kCaptureRuntimeFlagRecordingStartPending) == 0) {
            return RecordingStartIntent::Idle;
        }
        return (flags & kCaptureRuntimeFlagRecordingStartAudioOnly) != 0 ? RecordingStartIntent::AudioOnly
                                                                        : RecordingStartIntent::Video;
    }

    void SetRecordingStartIntent(RecordingStartIntent intent) {
        constexpr uint32_t intentMask =
            kCaptureRuntimeFlagRecordingStartPending | kCaptureRuntimeFlagRecordingStartAudioOnly;
        uint32_t intentFlags = 0;
        if (intent != RecordingStartIntent::Idle) {
            intentFlags |= kCaptureRuntimeFlagRecordingStartPending;
            if (intent == RecordingStartIntent::AudioOnly) {
                intentFlags |= kCaptureRuntimeFlagRecordingStartAudioOnly;
            }
        }

        uint32_t current = runtimeFlags.load(std::memory_order_acquire);
        for (;;) {
            const uint32_t desired = (current & ~intentMask) | intentFlags;
            if (runtimeFlags.compare_exchange_weak(current, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
                return;
            }
        }
    }

    void SetRuntimeFlag(uint32_t flag, bool enabled) {
        if (enabled) {
            runtimeFlags.fetch_or(flag, std::memory_order_acq_rel);
        } else {
            runtimeFlags.fetch_and(~flag, std::memory_order_acq_rel);
        }
    }

    void PublishScreenGrabTarget(uint32_t processId, int32_t adapterLuidLow, int32_t adapterLuidHigh, bool active) {
        screenGrabTargetSequence.fetch_add(1, std::memory_order_acq_rel);
        screenGrabTargetPid.store(active ? processId : 0, std::memory_order_relaxed);
        screenGrabAdapterLuidLow.store(active ? adapterLuidLow : 0, std::memory_order_relaxed);
        screenGrabAdapterLuidHigh.store(active ? adapterLuidHigh : 0, std::memory_order_relaxed);
        screenGrabTargetActive.store(active ? 1u : 0u, std::memory_order_release);
        screenGrabTargetSequence.fetch_add(1, std::memory_order_release);
    }

    bool ReadScreenGrabTarget(ScreenGrabTargetSnapshot& snapshot) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t before = screenGrabTargetSequence.load(std::memory_order_acquire);
            if ((before & 1u) != 0)
                continue;
            ScreenGrabTargetSnapshot candidate;
            candidate.processId = screenGrabTargetPid.load(std::memory_order_relaxed);
            candidate.adapterLuidLow = screenGrabAdapterLuidLow.load(std::memory_order_relaxed);
            candidate.adapterLuidHigh = screenGrabAdapterLuidHigh.load(std::memory_order_relaxed);
            candidate.active = screenGrabTargetActive.load(std::memory_order_acquire) != 0;
            const uint32_t after = screenGrabTargetSequence.load(std::memory_order_acquire);
            if (before == after && (after & 1u) == 0) {
                snapshot = candidate;
                return true;
            }
        }
        return false;
    }
};

// Frame slot for ring buffer
// Note: valid flag is atomic for proper cross-process visibility

#pragma pack(pop)
