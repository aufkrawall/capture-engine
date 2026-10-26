#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Resampling policy of the shader scaler (frame_scaler.h). API-neutral, so a
// D3D12 or Vulkan backend reuses it unchanged (llm-wiki/gpu-api-plan.md).
//
// Kernel: a separable Lanczos filter (2 lobes for quality=normal, 3 for
// quality=best). When downscaling it is stretched by the ratio, so its
// passband ends at the output's Nyquist frequency and fine detail cannot
// alias. When upscaling it runs at source pitch. `sharpness` scales the side
// lobes: 0 keeps only the positive main lobe (soft, ringing-free by
// construction), 100 the full windowed sinc.
//
// Anti-ringing: every pass clamps its result to the range of the source
// samples under the main lobe. The side lobes then steepen edges without
// halos, and an intermediate never leaves the source's value range, so UNORM
// intermediates lose nothing.
//
// Geometry: the whole source maps onto the whole output (the VideoProcessor's
// full source and destination rectangles), sample centres aligned, edges
// replicated.

namespace ce::video_scale {

struct KernelSpec {
    uint32_t lobes = 2;
    float sideLobeGain = 1.0f;  // 0..1
};

// [VideoScaling] quality ("best" -> 3 lobes, anything else 2) and sharpness (0..100).
KernelSpec KernelSpecFromConfig(const std::string& quality, int sharpness);
const char* DescribeKernel(const KernelSpec& spec);

// Kernel value at `x` source samples from the output sample's centre, in
// kernel units (before the downscale stretch); not normalized.
double KernelWeight(double x, const KernelSpec& spec);

// One axis. Output sample i reads source samples first[i] .. first[i] + taps - 1
// (clamped to the source by the shader) with weights[i * taps + k].
struct AxisTable {
    uint32_t inSize = 0;
    uint32_t outSize = 0;
    uint32_t taps = 0;  // a multiple of 4, the shader's vector width
    double stretch = 1.0;  // kernel stretch: max(1, inSize / outSize)
    std::vector<int32_t> first;
    // Main-lobe taps [lobeBegin, lobeEnd): the anti-ringing clamp's range.
    std::vector<uint32_t> lobeBegin;
    std::vector<uint32_t> lobeEnd;
    std::vector<float> weights;  // each output's weights sum to 1
};

// Empty table (taps == 0) for a zero size.
AxisTable BuildAxisTable(uint32_t inSize, uint32_t outSize, const KernelSpec& spec);

// GPU layout: per output one float4 header (first, lobeBegin, lobeEnd, 0),
// then taps / 4 float4 of weights.
std::vector<float> PackAxisTable(const AxisTable& table);
inline uint32_t PackedAxisStride(const AxisTable& table) {
    return 1u + table.taps / 4u;  // float4 elements per output sample
}

enum class PassOrder : uint8_t { kHorizontalFirst, kVerticalFirst };

// Intermediate storage. Anti-ringing keeps every value inside the source
// range, so UNORM storage only has to carry the precision: 10 bits for 8-bit
// sources or outputs, 16 bits otherwise.
enum class IntermediatePrecision : uint8_t { kUnorm10, kUnorm16 };

struct ScalePlan {
    bool scaleX = false;
    bool scaleY = false;
    PassOrder order = PassOrder::kHorizontalFirst;
    // Size of the first pass's output when both axes scale.
    uint32_t intermediateWidth = 0;
    uint32_t intermediateHeight = 0;
    IntermediatePrecision precision = IntermediatePrecision::kUnorm16;
    AxisTable horizontal;  // empty when !scaleX
    AxisTable vertical;    // empty when !scaleY
    uint64_t estimatedTaps = 0;  // source reads of both passes, for the log line
};

// `sourceBits`/`outputBits`: integer precision of the source and of the
// final output (8 or 10). The order with fewer source reads runs first.
ScalePlan PlanScale(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
                    const KernelSpec& spec, uint32_t sourceBits, uint32_t outputBits);

}  // namespace ce::video_scale
