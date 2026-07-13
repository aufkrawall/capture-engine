#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace ce::mux {

enum class HeaderValidationIssue : uint8_t {
    kNone = 0,
    kMissingStream,
    kMissingCodecParams,
    kInvalidTimeBase,
};

inline HeaderValidationIssue ValidateStreamForHeader(bool hasStream, bool hasCodecParams, int timeBaseNum,
                                                     int timeBaseDen) {
    if (!hasStream) {
        return HeaderValidationIssue::kMissingStream;
    }
    if (!hasCodecParams) {
        return HeaderValidationIssue::kMissingCodecParams;
    }
    if (timeBaseNum <= 0 || timeBaseDen <= 0) {
        return HeaderValidationIssue::kInvalidTimeBase;
    }
    return HeaderValidationIssue::kNone;
}

inline const char* HeaderValidationIssueToString(HeaderValidationIssue issue) {
    switch (issue) {
        case HeaderValidationIssue::kNone:
            return "none";
        case HeaderValidationIssue::kMissingStream:
            return "missing stream";
        case HeaderValidationIssue::kMissingCodecParams:
            return "missing codec parameters";
        case HeaderValidationIssue::kInvalidTimeBase:
            return "invalid time base";
    }
    return "unknown";
}

inline int64_t ComputeDurationDeltaUs(int64_t lhsUs, int64_t rhsUs) {
    return lhsUs >= rhsUs ? (lhsUs - rhsUs) : (rhsUs - lhsUs);
}

inline bool IsDurationWithinToleranceUs(int64_t lhsUs, int64_t rhsUs, int64_t toleranceUs = 0) {
    if (toleranceUs < 0) {
        toleranceUs = 0;
    }
    return ComputeDurationDeltaUs(lhsUs, rhsUs) <= toleranceUs;
}

inline int64_t CeilDurationUnitUs(int64_t numerator, int64_t denominator) {
    if (numerator <= 0 || denominator <= 0) {
        return 0;
    }
    return (numerator * 1000000LL + denominator - 1) / denominator;
}

inline int64_t ComputeAudioMuxRoundingToleranceUs(int sampleRate, int timeBaseNum, int timeBaseDen) {
    int64_t toleranceUs = 1;
    if (sampleRate > 0) {
        toleranceUs = std::max<int64_t>(toleranceUs, CeilDurationUnitUs(1, sampleRate));
    }
    if (timeBaseNum > 0 && timeBaseDen > 0) {
        toleranceUs = std::max<int64_t>(toleranceUs, CeilDurationUnitUs(timeBaseNum, timeBaseDen));
    }
    return toleranceUs;
}

inline int64_t ComputeCfrAudioLatticeFrameQuantum(int fps, const std::vector<int>& sampleRates) {
    if (fps <= 0) {
        return 0;
    }
    int64_t quantum = 1;
    for (int sampleRate : sampleRates) {
        if (sampleRate <= 0) {
            continue;
        }
        const int64_t rateQuantum = fps / std::gcd(fps, sampleRate);
        const int64_t divisor = std::gcd(quantum, rateQuantum);
        if (quantum > std::numeric_limits<int64_t>::max() / (rateQuantum / divisor)) {
            return 0;
        }
        quantum *= rateQuantum / divisor;
    }
    return quantum;
}

inline int64_t ComputeCfrAudioLatticeExtensionFrames(int64_t frameCount, int64_t frameQuantum) {
    if (frameCount < 0 || frameQuantum <= 0) {
        return 0;
    }
    const int64_t remainder = frameCount % frameQuantum;
    return remainder == 0 ? 0 : frameQuantum - remainder;
}

inline int64_t ChoosePostMuxStreamStartUs(int64_t streamStartUs, bool hasStreamStart, int64_t firstPacketStartUs,
                                          bool hasFirstPacketStart) {
    if (hasStreamStart && hasFirstPacketStart) {
        return std::min(streamStartUs, firstPacketStartUs);
    }
    if (hasStreamStart) {
        return streamStartUs;
    }
    if (hasFirstPacketStart) {
        return firstPacketStartUs;
    }
    return 0;
}

struct PacketTimelineStats {
    bool seen = false;
    uint32_t packetCount = 0;
    int64_t firstStartUs = 0;
    int64_t lastStartUs = 0;
    int64_t lastEndUs = 0;
    int64_t maxForwardStartGapUs = 0;
};

inline int64_t ComputePacketEndUs(int64_t packetStartUs, int64_t packetDurationUs) {
    return std::max<int64_t>(0, packetStartUs) + std::max<int64_t>(0, packetDurationUs);
}

inline void ObservePacketTimeline(PacketTimelineStats& stats, int64_t packetStartUs, int64_t packetDurationUs) {
    const int64_t clampedStartUs = std::max<int64_t>(0, packetStartUs);
    const int64_t packetEndUs = ComputePacketEndUs(packetStartUs, packetDurationUs);
    if (!stats.seen) {
        stats.seen = true;
        stats.firstStartUs = clampedStartUs;
        stats.lastStartUs = clampedStartUs;
    } else if (clampedStartUs >= stats.lastStartUs) {
        stats.maxForwardStartGapUs = std::max(stats.maxForwardStartGapUs, clampedStartUs - stats.lastStartUs);
        // Packet callbacks can be in decode order when B-frame reordering is
        // active. Keep the monotonic PTS frontier; a backtrack must not make
        // the next forward packet look like an artificial multi-tick gap.
        stats.lastStartUs = clampedStartUs;
    }
    stats.packetCount++;
    stats.lastEndUs = std::max(stats.lastEndUs, packetEndUs);
}

inline bool PacketTimelineExceedsTarget(const PacketTimelineStats& stats, int64_t targetDurationUs,
                                        int64_t toleranceUs = 0) {
    if (!stats.seen) {
        return false;
    }
    return stats.lastEndUs > targetDurationUs + std::max<int64_t>(0, toleranceUs);
}

}  // namespace ce::mux
