#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "packet_clamp_and_drift.h"

// When a started-but-quiet audio source counts as silence, and CFR pull/resync rules.

namespace ce::audio {

inline bool ShouldTreatInactiveStartedAppCaptureAsSilence(bool isCfrRecording, bool isAppAudioSource,
                                                          bool sourceHasStarted, bool captureActive) {
    // Once a process-loopback source has participated in the recording, its process may exit while
    // other sources on the same track continue. No more packets can arrive from an inactive client,
    // so waiting for a full pull chunk can only stall the whole track. Drain any buffered tail and
    // represent the rest of the absolute timeline as silence; a later capture epoch live-joins again.
    return isCfrRecording && isAppAudioSource && sourceHasStarted && !captureActive;
}

inline bool ShouldBootstrapPacketlessSourceAsSilence(bool isCfrRecording, bool sourceTimelineValid,
                                                     size_t bufferedRealSamples, int64_t targetTimelineSamples,
                                                     size_t requiredRealSamples) {
    return isCfrRecording && sourceTimelineValid && bufferedRealSamples == 0 && targetTimelineSamples > 0 &&
           static_cast<uint64_t>(targetTimelineSamples) >= static_cast<uint64_t>(requiredRealSamples);
}

inline bool ShouldTreatStartedTimelineSourceShortfallAsSilence(bool sparseStartedSourceMaySilence,
                                                               size_t bufferedTimelineSamples,
                                                               int64_t requestedSamples = 0,
                                                               int64_t partialSilenceThresholdSamples = 0) {
    if (!sparseStartedSourceMaySilence) {
        return false;
    }
    if (bufferedTimelineSamples == 0) {
        return true;
    }
    if (requestedSamples <= 0 || partialSilenceThresholdSamples <= 0 ||
        requestedSamples <= partialSilenceThresholdSamples) {
        return false;
    }
    return bufferedTimelineSamples < static_cast<size_t>(requestedSamples);
}

// A started source whose real content is merely LATE is not a silent source. It is
// still delivering packets; the consumer simply ran past the capture edge. Holding
// the pull (which only defers scheduling, never timeline placement) lets the
// producer overtake the cursor again instead of exporting silence and then
// destroying the real audio as timeline overlap. The hold is bounded by the
// reservoir cap so a genuinely stuck source can never freeze a co-mixed track.
inline bool ShouldHoldCfrAudioPullForLateLiveSource(bool isCfrRecording, bool forceDrain, bool sourceTimelineValid,
                                                    bool sourceBootstrapComplete, bool sourceCaptureActive,
                                                    bool sourceRecentlyDeliveredRealPackets, int64_t requestedSamples,
                                                    size_t bufferedTimelineSamples, int64_t reservoirExtraMs,
                                                    int64_t maxReservoirExtraMs = kAudioIngestMaxExtraReservoirMs) {
    if (!isCfrRecording || forceDrain || !sourceTimelineValid || !sourceBootstrapComplete || !sourceCaptureActive ||
        !sourceRecentlyDeliveredRealPackets || requestedSamples <= 0) {
        return false;
    }
    if (reservoirExtraMs >= std::max<int64_t>(0, maxReservoirExtraMs)) {
        return false;
    }

    return bufferedTimelineSamples < static_cast<size_t>(requestedSamples);
}

// Last resort. Only reachable when the reservoir is already at its cap and a live
// source has still had every packet destroyed as timeline overlap for this long.
// Re-anchoring costs that source a one-time content skip equal to the unrecoverable
// deficit; the alternative is silence for the rest of the recording. Track lengths,
// PTS, and every other source stay untouched.
inline bool ShouldResyncStarvedLiveAudioSource(bool isCfrRecording, bool forceDrain, bool reservoirAtCap,
                                               bool sourceRecentlyDeliveredRealPackets, int64_t starvedElapsedMs,
                                               int64_t deficitSamples, int64_t minStarvedMs = 1500,
                                               int64_t minDeficitSamples = 480) {
    return isCfrRecording && !forceDrain && reservoirAtCap && sourceRecentlyDeliveredRealPackets &&
           starvedElapsedMs >= std::max<int64_t>(0, minStarvedMs) &&
           deficitSamples >= std::max<int64_t>(1, minDeficitSamples);
}

inline PacketTimelineAdjustment ComputePacketTimelineAdjustment(int64_t packetStartSamples,
                                                                int64_t writtenTimelineSamples,
                                                                int64_t slopSamples = 0) {
    PacketTimelineAdjustment adjustment;
    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedWrittenTimelineSamples = std::max<int64_t>(0, writtenTimelineSamples);
    const int64_t deltaSamples = clampedPacketStartSamples - clampedWrittenTimelineSamples;
    if (std::abs(deltaSamples) <= std::max<int64_t>(0, slopSamples)) {
        return adjustment;
    }

    if (deltaSamples > 0) {
        adjustment.gapSamples = deltaSamples;
    } else {
        adjustment.overlapSamples = -deltaSamples;
    }
    return adjustment;
}

// A single packet's leading-silence timeline gap can never legitimately exceed
// what the destination ring buffer can retain: WriteRetainNew drops the oldest
// samples to make room, so any silence beyond the capacity is discarded the moment
// it is written. Bounding the gap to the ring capacity keeps a corrupt or
// out-of-domain packet timestamp from sizing a pathological silence allocation
// (the 192 kHz-loopback bad_alloc crash) while leaving every in-range gap
// untouched. ringCapacitySamples <= 0 disables the bound.
inline int64_t ClampTimelineGapSamplesToCapacity(int64_t gapSamples, int64_t ringCapacitySamples) {
    if (gapSamples <= 0) {
        return 0;
    }
    if (ringCapacitySamples <= 0) {
        return gapSamples;
    }
    return std::min<int64_t>(gapSamples, ringCapacitySamples);
}

inline int64_t ComputeStartupFirstPacketRebaseOffset(int64_t packetStartSamples, bool sawSyncPendingPackets,
                                                     int64_t cappedStartupGapSamples, int64_t rebaseThresholdSamples) {
    if (!sawSyncPendingPackets) {
        return 0;
    }

    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedCappedGapSamples = std::max<int64_t>(0, cappedStartupGapSamples);
    const int64_t clampedThresholdSamples = std::max<int64_t>(clampedCappedGapSamples, rebaseThresholdSamples);
    if (clampedPacketStartSamples < clampedThresholdSamples || clampedPacketStartSamples <= clampedCappedGapSamples) {
        return 0;
    }

    return clampedPacketStartSamples - clampedCappedGapSamples;
}

inline int64_t ComputeSharedStartupFirstPacketRebaseOffset(int64_t earliestPacketStartSamples,
                                                           int64_t cappedStartupGapSamples,
                                                           int64_t rebaseThresholdSamples) {
    return ComputeStartupFirstPacketRebaseOffset(earliestPacketStartSamples, true, cappedStartupGapSamples,
                                                 rebaseThresholdSamples);
}

inline int64_t ApplyStartupPacketTimelineRebaseOffset(int64_t packetStartSamples, int64_t startupRebasedGapSamples) {
    const int64_t clampedPacketStartSamples = std::max<int64_t>(0, packetStartSamples);
    const int64_t clampedRebasedGapSamples = std::max<int64_t>(0, startupRebasedGapSamples);
    if (clampedRebasedGapSamples <= 0) {
        return clampedPacketStartSamples;
    }

    return std::max<int64_t>(0, clampedPacketStartSamples - clampedRebasedGapSamples);
}

inline bool ShouldPreservePendingAudioPacketsForStartupSync(bool isWgcCfrRecording, int64_t wgcStartupExtraDelayQpc) {
    return isWgcCfrRecording && wgcStartupExtraDelayQpc > 0;
}

inline PacketTimelineAdjustment ComputeStartupAwarePacketTimelineAdjustment(
    int64_t packetStartSamples, int64_t writtenTimelineSamples, int64_t steadyStateSlopSamples,
    int64_t startupWindowSamples, int64_t startupSlopSamples, int64_t startupOverlapTrimThresholdSamples) {
    const int64_t startupBoundarySamples =
        std::max(std::max<int64_t>(0, packetStartSamples), std::max<int64_t>(0, writtenTimelineSamples));
    const bool startupSettling = startupBoundarySamples < std::max<int64_t>(0, startupWindowSamples);

    PacketTimelineAdjustment adjustment = ComputePacketTimelineAdjustment(
        packetStartSamples, writtenTimelineSamples, startupSettling ? startupSlopSamples : steadyStateSlopSamples);
    if (startupSettling && adjustment.overlapSamples > 0 &&
        adjustment.overlapSamples < std::max<int64_t>(0, startupOverlapTrimThresholdSamples)) {
        adjustment.overlapSamples = 0;
    }

    return adjustment;
}

// Smooth soft-knee limiter for the final per-track mix. Samples with magnitude
// at or below `knee` pass through bit-exact; above the knee the excess is mapped
// through x/(1+x) so the output asymptotically approaches (but never reaches)
// +-1.0. This guarantees no hard clip / no clipped click at the encoder input
// while keeping in-range audio fully transparent (important for lossless codecs).
//
// NOTE: deliberately contains NO additional global waveshaper (e.g. tanh). An
// unconditional tanh squashes and harmonically distorts every sample even when
// far below full scale, which audibly thins the audio. The soft-knee alone is
// sufficient to bound the signal.
// Stable identity for an app-audio capture targeting a specific track. Two
// app-audio sources that resolve to the same process and feed the same track
// would capture identical audio twice; summing those independently buffered,
// phase-offset streams into one track causes comb-filter ("metallic") artifacts.
// Process name is matched case-insensitively; an explicit PID is used when no
// name is set.
inline std::string AppAudioTrackIdentity(const std::string& processName, unsigned long processId, int track) {
    std::string id;
    if (!processName.empty()) {
        id = processName;
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    } else {
        id = "pid:" + std::to_string(processId);
    }
    return id + "#" + std::to_string(track);
}

inline void ApplySoftKneeLimiter(float* buffer, size_t count, float knee = 0.9f) {
    if (!buffer || count == 0) {
        return;
    }
    const float range = 1.0f - knee;
    if (range <= 0.0f) {
        return;
    }
    const float scale = 1.0f / range;
    for (size_t i = 0; i < count; ++i) {
        float s = buffer[i];
        if (s > knee) {
            const float excess = (s - knee) * scale;
            buffer[i] = knee + range * (excess / (1.0f + excess));
        } else if (s < -knee) {
            const float excess = (-s - knee) * scale;
            buffer[i] = -(knee + range * (excess / (1.0f + excess)));
        }
        // else: |s| <= knee -> untouched (bit-exact passthrough)
    }
}

}  // namespace ce::audio
