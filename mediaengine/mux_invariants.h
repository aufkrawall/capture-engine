#pragma once

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

}  // namespace ce::mux