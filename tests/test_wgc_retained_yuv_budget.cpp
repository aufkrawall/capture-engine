#include <gtest/gtest.h>

#include <cstdint>

#include "common/capture/capture_pipeline_policy.h"

namespace policy = ce::capture_policy;

// YUV-retained smoothness reservoir (llm-wiki/wgc-reservoir-vram-design.md):
// the budget must size planar NV12/P010 slots by their real footprint, keep the
// slot count the 300 ms target needs, and stop HDR from being budget-capped.

namespace {

constexpr uint32_t kFp16Bits = 64;
constexpr uint32_t kRgb32Bits = 32;
constexpr uint32_t kP010Bits = 24;
constexpr uint32_t kNv12Bits = 12;
constexpr uint64_t kMiB = 1024ull * 1024ull;

}  // namespace

TEST(WgcRetainedYuvBudgetTest, PlanarSurfaceBytesMatchTheirPlanes) {
    // NV12: W*H luma bytes + W*H/2 interleaved chroma bytes; P010 doubles both.
    EXPECT_EQ(policy::EstimateWgcSurfaceBytesForBits(3840, 2160, kNv12Bits), 3840ull * 2160ull * 3ull / 2ull);
    EXPECT_EQ(policy::EstimateWgcSurfaceBytesForBits(3840, 2160, kP010Bits), 3840ull * 2160ull * 3ull);
    EXPECT_EQ(policy::EstimateWgcSurfaceBytesForBits(3840, 2160, kRgb32Bits),
              policy::EstimateWgcSurfaceBytes(3840, 2160, 4));
    EXPECT_EQ(policy::EstimateWgcSurfaceBytesForBits(0, 2160, kP010Bits), 0ull);
}

TEST(WgcRetainedYuvBudgetTest, ByteApiIsTheBitsApiTimesEight) {
    const auto bytes = policy::ComputeWgcSmoothnessSurfaceBudget(120, 300, 3840, 2160, 8, 4, 3000, 4, true);
    const auto bits =
        policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kFp16Bits, kRgb32Bits, 3000, 4, true);
    EXPECT_EQ(bytes.copyPoolSlots, bits.copyPoolSlots);
    EXPECT_EQ(bytes.sourceFramePoolBuffers, bits.sourceFramePoolBuffers);
    EXPECT_EQ(bytes.estimatedBytes, bits.estimatedBytes);
    EXPECT_EQ(bytes.retainedExtraFrames, bits.retainedExtraFrames);
}

TEST(WgcRetainedYuvBudgetTest, DuplicationTenBitKeepsSlotsAndDropsAQuarter) {
    // dxgi_dup 4K120 10-bit (session 015331 r0002): 64 R10 slots = 2025 MiB.
    const auto r10 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kRgb32Bits, kRgb32Bits,
                                                                      3000, 4, false);
    const auto p010 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kRgb32Bits, kP010Bits,
                                                                       3000, 4, false);
    EXPECT_EQ(r10.copyPoolSlots, 64u);
    EXPECT_EQ(p010.copyPoolSlots, r10.copyPoolSlots);
    EXPECT_EQ(p010.retainedExtraFrames, r10.retainedExtraFrames);
    EXPECT_FALSE(p010.capLimited);
    EXPECT_EQ(r10.copyEstimatedBytes / kMiB, 2025ull);
    EXPECT_EQ(p010.copyEstimatedBytes / kMiB, 1518ull);  // 1518.75 MiB
    EXPECT_EQ(p010.copyEstimatedBytes * 4ull, r10.copyEstimatedBytes * 3ull);
}

TEST(WgcRetainedYuvBudgetTest, DuplicationEightBitNv12KeepsSlots) {
    const auto bgra = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kRgb32Bits, kRgb32Bits,
                                                                       3000, 4, false);
    const auto nv12 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kRgb32Bits, kNv12Bits,
                                                                       3000, 4, false);
    EXPECT_EQ(nv12.copyPoolSlots, bgra.copyPoolSlots);
    EXPECT_EQ(nv12.copyEstimatedBytes * 8ull, bgra.copyEstimatedBytes * 3ull);
}

TEST(WgcRetainedYuvBudgetTest, HdrFp16RetentionIsCapLimitedAndP010IsNot) {
    // WGC HDR today retains FP16: 47 FP16 surfaces fit 3000 MB, so the 300 ms
    // target cannot be held at 4K120. P010 retention reaches it.
    const auto fp16 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kFp16Bits, kFp16Bits,
                                                                       3000, 5, true);
    EXPECT_TRUE(fp16.capLimited);
    EXPECT_LT(fp16.retainedExtraFrames, fp16.desiredExtraFrames);

    const auto p010 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kFp16Bits, kP010Bits,
                                                                       3000, 5, true);
    EXPECT_FALSE(p010.capLimited);
    EXPECT_EQ(p010.retainedExtraFrames, p010.desiredExtraFrames);
    EXPECT_EQ(p010.copyPoolSlots, 64u);
    EXPECT_LT(p010.estimatedBytes, fp16.estimatedBytes);

    const auto dupFp16 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kFp16Bits, kFp16Bits,
                                                                          3000, 5, false);
    const auto dupP010 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kFp16Bits, kP010Bits,
                                                                          3000, 5, false);
    EXPECT_TRUE(dupFp16.capLimited);
    EXPECT_FALSE(dupP010.capLimited);
}

TEST(WgcRetainedYuvBudgetTest, WgcCompactSourcePoolStaysEightBuffersAt120Fps) {
    const auto p010 = policy::ComputeWgcSmoothnessSurfaceBudgetForBits(120, 300, 3840, 2160, kFp16Bits, kP010Bits,
                                                                       3000, 4, true);
    EXPECT_EQ(p010.sourceFramePoolBuffers, 8u);
    EXPECT_EQ(p010.sourceEstimatedBytes, 8ull * policy::EstimateWgcSurfaceBytesForBits(3840, 2160, kFp16Bits));
}
