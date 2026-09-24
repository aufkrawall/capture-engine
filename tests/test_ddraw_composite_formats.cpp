#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/ddraw_present_policy.h"
#include "source_fragment_reader.h"

// The CPU overlay composite's surface-format policy. 8-bit palettized is THE
// late-90s DirectDraw fullscreen mode and 24-bit the cheap true-color one;
// both used to be rejected outright ("cannot write a %u-bit presented
// surface", every frame, zero overlay pixels). The decision table is pinned
// because a widened format must never widen into a guess: YUV and unusual
// channel layouts stay rejected exactly as before.
namespace {

namespace policy = ce::ddraw_present_policy;

policy::SurfacePixelFormat RgbFormat(uint32_t bitCount, uint32_t red, uint32_t green, uint32_t blue,
                                     uint32_t alpha = 0) {
    policy::SurfacePixelFormat format;
    format.rgb = true;
    format.bitCount = bitCount;
    format.redMask = red;
    format.greenMask = green;
    format.blueMask = blue;
    format.alphaMask = alpha;
    return format;
}

}  // namespace

TEST(DdrawCompositeFormat, SurfaceEncodingDecisionTable) {
    // Standard 32-bit ARGB8888/RGB888 - with or without an alpha channel.
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu)),
              policy::SurfaceEncoding::Rgb888);
    policy::SurfacePixelFormat alpha888 = RgbFormat(32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
    alpha888.alphaPixels = true;
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(alpha888), policy::SurfaceEncoding::Rgb888);

    // 24-bit BGR888 - the other standard DirectDraw true-color layout.
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(24, 0x00FF0000u, 0x0000FF00u, 0x000000FFu)),
              policy::SurfaceEncoding::Rgb24);

    // 16/15-bit packed.
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(16, 0xF800u, 0x07E0u, 0x001Fu)),
              policy::SurfaceEncoding::Rgb565);
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(16, 0x7C00u, 0x03E0u, 0x001Fu)),
              policy::SurfaceEncoding::Rgb555);
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(15, 0x7C00u, 0x03E0u, 0x001Fu)),
              policy::SurfaceEncoding::Rgb555);

    // 8-bit palettized; 4-bit has no composite path and stays rejected.
    policy::SurfacePixelFormat palette8;
    palette8.rgb = true;
    palette8.paletteIndexed = true;
    palette8.bitCount = 8;
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(palette8), policy::SurfaceEncoding::Palette8);
    policy::SurfacePixelFormat palette4 = palette8;
    palette4.bitCount = 4;
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(palette4), policy::SurfaceEncoding::Unsupported);

    // YUV (UYVY and friends carry no DDPF_RGB) and unusual channel layouts
    // are refused rather than written with guessed channels.
    policy::SurfacePixelFormat yuv;
    yuv.bitCount = 16;
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(yuv), policy::SurfaceEncoding::Unsupported);
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(32, 0x000000FFu, 0x0000FF00u, 0x00FF0000u)),
              policy::SurfaceEncoding::Unsupported);  // swapped R/B masks
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(16, 0x7C00u, 0x03E0u, 0x001Fu, 0x8000u)),
              policy::SurfaceEncoding::Unsupported);  // 1555 with an alpha bit
    EXPECT_EQ(policy::ClassifySurfacePixelFormat(RgbFormat(32, 0x0FF00000u, 0x000FF000u, 0x00000FF0u)),
              policy::SurfaceEncoding::Unsupported);  // 12-bit channels
}

TEST(DdrawCompositeFormat, Rgb24RoundTripKeepsTheCanonicalBytes) {
    // 24-bit BGR memory bytes are the canonical value's low three bytes, so
    // pack and expand are the same mask in both directions and the composite's
    // proof compare sees exactly what the next lock reads back.
    const uint32_t canonical = 0xFF12AB34u;
    const uint32_t packed = policy::PackRgb24(canonical);
    EXPECT_EQ(packed, 0x0012AB34u);
    EXPECT_EQ(policy::ExpandRgb24(packed), canonical);
    // Expansion forces the opaque alpha the composite works in.
    EXPECT_EQ(policy::ExpandRgb24(0x00FFFFFFu), 0xFFFFFFFFu);
}

TEST(DdrawCompositeFormat, PaletteExpansionAndContractionFollowTheSnapshot) {
    const uint32_t palette[] = {0xFF102030u, 0xFF405060u, 0xFFE0E0E0u};

    // Exact hits round-trip: the application's own pixels survive expansion
    // and contraction unchanged (the composite's proof compare depends on it).
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t expanded = policy::ExpandPaletteIndex(i, palette, 3);
        EXPECT_EQ(policy::NearestPaletteIndex(expanded, palette, 3), i);
    }

    // Out-of-range indices expand to opaque black instead of reading past the
    // snapshot.
    EXPECT_EQ(policy::ExpandPaletteIndex(3, palette, 3), 0xFF000000u);
    EXPECT_EQ(policy::ExpandPaletteIndex(0, nullptr, 3), 0xFF000000u);

    // A blended overlay colour contracts to the nearest entry; duplicated
    // colours contract to the first entry, deterministically.
    EXPECT_EQ(policy::NearestPaletteIndex(0xFF405061u, palette, 3), 1u);
    const uint32_t duplicates[] = {0xFF000000u, 0xFF000000u, 0xFFFFFFFFu};
    EXPECT_EQ(policy::NearestPaletteIndex(0xFF000000u, duplicates, 3), 0u);

    // Palette invalidation: the helpers derive everything from the snapshot
    // passed in and keep nothing between calls, so the snapshot taken after
    // the application changed its palette (SetPalette, or the SetEntries of an
    // 8-bit fade) is the only palette that exists.
    const uint32_t faded[] = {0xFF081018u, 0xFF202830u, 0xFF707070u};
    EXPECT_EQ(policy::ExpandPaletteIndex(2, palette, 3), 0xFFE0E0E0u);
    EXPECT_EQ(policy::ExpandPaletteIndex(2, faded, 3), 0xFF707070u);
    EXPECT_NE(policy::ExpandPaletteIndex(2, palette, 3), policy::ExpandPaletteIndex(2, faded, 3));
    EXPECT_EQ(policy::NearestPaletteIndex(0xFFE0E0E0u, faded, 3), 2u);
}

TEST(DdrawCompositeFormat, TheCompositeFetchesThePalettePerWritePass) {
    // No palette state may outlive a write pass: an 8-bit palette fade mutates
    // entries through IDirectDrawPalette::SetEntries with no SetPalette call
    // anywhere, so only a per-pass GetEntries snapshot cannot go stale. The
    // policy helpers taking the snapshot as a parameter is the other half of
    // that guarantee.
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook/apis" / "ddraw_hook_overlay_composite.cpp";
    const std::string contents = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(contents.empty()) << source.string();
    EXPECT_NE(contents.find("FetchSurfacePalette(surface"), std::string::npos);
    EXPECT_NE(contents.find("GetEntries(0, 0, 256, entries)"), std::string::npos);
    // The quantization memo is bounded by the same pass it serves.
    EXPECT_NE(contents.find("paletteQuantizeCache.clear()"), std::string::npos);
}
