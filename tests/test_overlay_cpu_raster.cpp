/**
 * CPU rasterizer for the overlay draw list.
 *
 * The DirectDraw route has no GPU path into a DirectDraw surface, so producing
 * the overlay's pixels on the GPU costs a blocking readback on every
 * presentation. These tests pin the rasterizer's output format - premultiplied
 * BGRA - and the one-step blend that puts it into a surface.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../hook/common/ddraw_present_policy.h"
#include "../hook/common/overlay_cpu_raster.h"

namespace raster = ce::overlay_cpu_raster;
namespace policy = ce::ddraw_present_policy;

namespace {

// One axis-aligned quad, the shape every overlay rectangle and glyph takes.
void AppendQuad(std::vector<CustomOverlay::DrawVertex>& vertices, std::vector<uint16_t>& indices, float x, float y,
                float w, float h, uint32_t abgr) {
    const auto base = static_cast<uint16_t>(vertices.size());
    vertices.push_back({x, y, 0.0f, 0.0f, abgr});
    vertices.push_back({x + w, y, 1.0f, 0.0f, abgr});
    vertices.push_back({x + w, y + h, 1.0f, 1.0f, abgr});
    vertices.push_back({x, y + h, 0.0f, 1.0f, abgr});
    for (uint16_t offset : {0, 1, 2, 0, 2, 3})
        indices.push_back(static_cast<uint16_t>(base + offset));
}

raster::Target MakeTarget(int left, int top, int width, int height) {
    raster::Target target;
    target.left = left;
    target.top = top;
    target.width = width;
    target.height = height;
    return target;
}

}  // namespace

TEST(OverlayCpuRasterTest, AnOpaqueQuadFillsItsOwnPixelsAndNothingElse) {
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 2.0f, 2.0f, 4.0f, 4.0f, 0xFF0000FFu);  // opaque red in ABGR
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, false}};

    std::vector<uint32_t> out;
    ASSERT_TRUE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(0, 0, 8, 8), out));
    ASSERT_EQ(out.size(), 64u);

    // Inside: opaque red, stored premultiplied as ARGB.
    EXPECT_EQ(out[3 * 8 + 3], 0xFFFF0000u);
    // Outside stays fully transparent, so the blend leaves the frame alone.
    EXPECT_EQ(out[0], 0u);
    EXPECT_EQ(out[7 * 8 + 7], 0u);
}

TEST(OverlayCpuRasterTest, TheTargetRectangleOffsetsTheGeometry) {
    // The rasterizer writes a region of the frame, not the whole frame, so the
    // draw list's viewport coordinates are relative to the target's corner.
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 64.0f, 64.0f, 4.0f, 4.0f, 0xFF00FF00u);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, false}};

    std::vector<uint32_t> out;
    ASSERT_TRUE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(64, 64, 8, 8), out));
    EXPECT_NE(out[1 * 8 + 1], 0u);
    EXPECT_EQ(out[7 * 8 + 7], 0u);
}

TEST(OverlayCpuRasterTest, AHalfTransparentQuadIsStoredPremultiplied) {
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    // 50% alpha, full blue in ABGR (0xAABBGGRR -> alpha 0x80, blue 0xFF).
    AppendQuad(vertices, indices, 0.0f, 0.0f, 4.0f, 4.0f, 0x80FF0000u);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, false}};

    std::vector<uint32_t> out;
    ASSERT_TRUE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(0, 0, 4, 4), out));
    const uint32_t pixel = out[0];
    EXPECT_EQ((pixel >> 24) & 0xFFu, 0x80u);
    // Blue premultiplied by 0x80/255 is about half.
    const uint32_t blue = pixel & 0xFFu;
    EXPECT_GE(blue, 0x7Du);
    EXPECT_LE(blue, 0x83u);
}

TEST(OverlayCpuRasterTest, AnEmptyDrawListRasterizesNothing) {
    std::vector<uint32_t> out;
    EXPECT_FALSE(raster::Rasterize({}, {}, {}, raster::FontAtlasView{}, MakeTarget(0, 0, 8, 8), out));
}

TEST(OverlayCpuRasterTest, AZeroSizedTargetIsRejected) {
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 0.0f, 0.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, false}};
    std::vector<uint32_t> out;
    EXPECT_FALSE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(0, 0, 0, 8), out));
}

TEST(OverlayCpuRasterTest, OutOfRangeIndicesAreIgnoredRatherThanRead) {
    std::vector<CustomOverlay::DrawVertex> vertices{{0.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu}};
    std::vector<uint16_t> indices{0, 9, 9};
    std::vector<CustomOverlay::DrawCommand> commands{{0, 1, 0, 3, false}};
    std::vector<uint32_t> out;
    EXPECT_FALSE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(0, 0, 4, 4), out));
}

TEST(OverlayCpuRasterTest, ACommandReachingPastTheIndexBufferIsIgnored) {
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 0.0f, 0.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 600, false}};
    std::vector<uint32_t> out;
    EXPECT_FALSE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(0, 0, 4, 4), out));
}

// ---------------------------------------------------------------------------
// The one blend step that puts the sprite into a DirectDraw surface.
// ---------------------------------------------------------------------------

TEST(OverlayCpuRasterTest, AFullyTransparentSpritePixelLeavesTheFrameAlone) {
    EXPECT_EQ(policy::BlendPremultipliedOver(0x00000000u, 0xFF123456u), 0xFF123456u);
}

TEST(OverlayCpuRasterTest, AnOpaqueSpritePixelReplacesTheFrame) {
    EXPECT_EQ(policy::BlendPremultipliedOver(0xFFAABBCCu, 0xFF123456u), 0xFFAABBCCu);
}

TEST(OverlayCpuRasterTest, AHalfCoveringSpritePixelMixesBothHalves) {
    // Premultiplied half-white over black is about half grey.
    const uint32_t blended = policy::BlendPremultipliedOver(0x80808080u, 0xFF000000u);
    const uint32_t red = (blended >> 16) & 0xFFu;
    EXPECT_GE(red, 0x7Du);
    EXPECT_LE(red, 0x83u);
    EXPECT_EQ((blended >> 24) & 0xFFu, 0xFFu);
}

TEST(OverlayCpuRasterTest, BlendingIsIdempotentForAnOpaqueSprite) {
    // Repeat presentations blend the same sprite again; an opaque pixel must not
    // drift, which is what a read-modify-write GPU composite could not promise.
    const uint32_t once = policy::BlendPremultipliedOver(0xFF204080u, 0xFF000000u);
    const uint32_t twice = policy::BlendPremultipliedOver(0xFF204080u, once);
    EXPECT_EQ(once, twice);
}

TEST(OverlayCpuRasterTest, TheSpriteCacheHoldsUntilGeometryOrRectangleMoves) {
    EXPECT_TRUE(policy::OverlaySpriteIsCurrent(true, true, 42u, 42u));
    EXPECT_FALSE(policy::OverlaySpriteIsCurrent(true, true, 42u, 43u));
    EXPECT_FALSE(policy::OverlaySpriteIsCurrent(true, false, 42u, 42u));
    EXPECT_FALSE(policy::OverlaySpriteIsCurrent(false, true, 42u, 42u));
}
