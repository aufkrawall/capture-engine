#pragma once

#include <algorithm>
#include <cstdint>

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

inline int64_t ComputeAudioCodecPrimingToleranceUs(int initialPaddingSamples, int sampleRate,
                                                   int64_t roundingToleranceUs) {
    if (initialPaddingSamples <= 0 || sampleRate <= 0) {
        return 0;
    }
    const int64_t primingUs =
        (static_cast<int64_t>(initialPaddingSamples) * 1000000LL + static_cast<int64_t>(sampleRate) - 1LL) /
        static_cast<int64_t>(sampleRate);
    return primingUs + std::max<int64_t>(0, roundingToleranceUs);
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
    }
    stats.packetCount++;
    stats.lastStartUs = clampedStartUs;
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
