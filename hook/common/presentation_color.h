#pragma once

namespace ce::presentation_color {

// Describes the pixels stored in a presentation image. The storage format is
// deliberately not part of this enum: R10G10B10A2 can carry either SDR/709 or
// HDR10/PQ, and FP16 only means scRGB when the presentation contract says so.
enum class Encoding {
    Sdr709,
    LinearScRgb,
    Hdr10Pq,
    Unsupported,
};

inline bool IsHDR(Encoding encoding) {
    return encoding == Encoding::LinearScRgb || encoding == Encoding::Hdr10Pq;
}

inline int OverlayMode(Encoding encoding) {
    switch (encoding) {
        case Encoding::LinearScRgb:
            return 1;
        case Encoding::Hdr10Pq:
            return 2;
        default:
            return 0;
    }
}

inline const char* Describe(Encoding encoding) {
    switch (encoding) {
        case Encoding::Sdr709:
            return "SDR-G22-P709";
        case Encoding::LinearScRgb:
            return "scRGB-linear-P709";
        case Encoding::Hdr10Pq:
            return "HDR10-PQ-P2020";
        default:
            return "unsupported";
    }
}

}  // namespace ce::presentation_color
