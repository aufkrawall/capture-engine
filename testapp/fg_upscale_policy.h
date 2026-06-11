// Pure-logic upscaling policy for the FG test apps: quality-mode <-> scale mapping, render-size
// computation, and the Halton(2,3) camera jitter sequence. No D3D/SDK types so
// tests/test_fg_upscale_policy.cpp can exercise the exact production logic.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace testapp {
namespace fg {

// Matches the FFX upscale quality modes; DLSS maps NativeAA->eDLAA, Quality->eMaxQuality,
// Balanced->eBalanced, Performance->eMaxPerformance, UltraPerformance->eUltraPerformance.
enum class UpscaleQuality : uint32_t {
    NativeAA = 0,
    Quality = 1,
    Balanced = 2,
    Performance = 3,
    UltraPerformance = 4,
};

inline const char* UpscaleQualityName(UpscaleQuality quality) {
    switch (quality) {
        case UpscaleQuality::NativeAA:
            return "nativeaa";
        case UpscaleQuality::Quality:
            return "quality";
        case UpscaleQuality::Balanced:
            return "balanced";
        case UpscaleQuality::Performance:
            return "performance";
        case UpscaleQuality::UltraPerformance:
            return "ultraperformance";
        default:
            return "unknown";
    }
}

inline bool ParseUpscaleQuality(const char* text, UpscaleQuality* out) {
    if (!text || !out) {
        return false;
    }
    const UpscaleQuality all[] = {UpscaleQuality::NativeAA, UpscaleQuality::Quality, UpscaleQuality::Balanced,
                                  UpscaleQuality::Performance, UpscaleQuality::UltraPerformance};
    for (UpscaleQuality quality : all) {
        if (std::strcmp(text, UpscaleQualityName(quality)) == 0) {
            *out = quality;
            return true;
        }
    }
    return false;
}

// Per-dimension upscaling ratio (display / render), the FSR convention shared by DLSS in practice.
inline float UpscaleRatio(UpscaleQuality quality) {
    switch (quality) {
        case UpscaleQuality::Quality:
            return 1.5f;
        case UpscaleQuality::Balanced:
            return 1.7f;
        case UpscaleQuality::Performance:
            return 2.0f;
        case UpscaleQuality::UltraPerformance:
            return 3.0f;
        case UpscaleQuality::NativeAA:
        default:
            return 1.0f;
    }
}

struct RenderSize {
    unsigned width;
    unsigned height;
};

// Render resolution for a display resolution and quality mode. scalePercentOverride > 0 (percent of
// display resolution per dimension, e.g. 66) takes precedence over the quality ratio. Dimensions
// are floored to even values (NV12-style alignment keeps every consumer happy) and clamped to
// [2, display]; a ratio of 1.0 (or override >= 100) returns the display size exactly.
inline RenderSize ComputeRenderSize(unsigned displayWidth, unsigned displayHeight, UpscaleQuality quality,
                                    int scalePercentOverride) {
    double ratio = static_cast<double>(UpscaleRatio(quality));
    if (scalePercentOverride > 0) {
        ratio = 100.0 / static_cast<double>(scalePercentOverride);
    }
    if (ratio <= 1.0) {
        return {displayWidth, displayHeight};
    }
    auto scaleDim = [ratio](unsigned displayDim) -> unsigned {
        unsigned dim = static_cast<unsigned>(static_cast<double>(displayDim) / ratio);
        dim &= ~1u;
        if (dim < 2) {
            dim = 2;
        }
        if (dim > displayDim) {
            dim = displayDim;
        }
        return dim;
    };
    return {scaleDim(displayWidth), scaleDim(displayHeight)};
}

// Radical-inverse Halton sequence (1-based index), value in [0, 1).
inline float HaltonSequence(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

// FSR jitter phase-count formula: 8 * (display/render)^2, also applied to DLSS and TAA (DLSS's
// recommendation is the same shape). Native (ratio 1.0) yields the base phase count of 8.
inline int JitterPhaseCount(unsigned renderWidth, unsigned displayWidth) {
    if (renderWidth == 0 || displayWidth == 0) {
        return 8;
    }
    const double ratio = static_cast<double>(displayWidth) / static_cast<double>(renderWidth);
    return static_cast<int>(std::ceil(8.0 * ratio * ratio));
}

struct JitterOffset {
    float x;
    float y;
};

// Sub-pixel camera jitter in pixels, centered around zero: [-0.5, 0.5) per axis, Halton(2,3).
inline JitterOffset ComputeJitter(uint64_t frameIndex, int phaseCount) {
    if (phaseCount < 1) {
        phaseCount = 1;
    }
    const uint32_t index = static_cast<uint32_t>(frameIndex % static_cast<uint64_t>(phaseCount)) + 1u;
    return {HaltonSequence(index, 2) - 0.5f, HaltonSequence(index, 3) - 0.5f};
}

}  // namespace fg
}  // namespace testapp
