#include <gtest/gtest.h>

#include <cstdint>

#include "hook/d3d11/d3d11_af_engine.h"

// The forced-AF engine's single-writer counters (hook/d3d11/d3d11_af_engine.h). The x86 hook DLL
// targets i686 without SSE2, where a 64-bit atomic increment costs 40 cycles against 0.7 for 32
// bits (BioShock Infinite counted several per bind, pass and sampler creation), so 32-bit hooks
// count with 32 bits and every window is taken modulo the counter's width.
TEST(D3D11AFCountTest, CountsWithThePointerWidth) {
    EXPECT_EQ(sizeof(ce::d3d11_af::CountValue), sizeof(void*) >= 8 ? 8u : 4u);
    ce::d3d11_af::Count count;
    count += 5;
    ++count;
    EXPECT_EQ(count.Load(), 6u);
}

TEST(D3D11AFCountTest, WindowsSurviveAWrap) {
    // A 32-bit count that wrapped during the window.
    EXPECT_EQ(ce::d3d11_af::CountDelta<uint32_t>(3u, 0xFFFFFFFEull), 5u);
    // Sums of wrapped counts (retired contexts plus live ones) still give the window: one count
    // went 0xFFFFFFF0 -> 0x4 (+0x14), the other 0x10 -> 0x20 (+0x10).
    const uint64_t before = 0xFFFFFFF0ull + 0x10ull;
    const uint64_t now = 0x00000004ull + 0x20ull;
    EXPECT_EQ(ce::d3d11_af::CountDelta<uint32_t>(now, before), 0x24u);
    EXPECT_EQ(ce::d3d11_af::CountDelta<uint64_t>(10u, 4u), 6u);
}
