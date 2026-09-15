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

CustomOverlay::DrawCommand MakeMergedCommand(const std::vector<CustomOverlay::DrawVertex>& vertices,
                                             const std::vector<uint16_t>& indices) {
    return {0, static_cast<uint32_t>(vertices.size()), 0, static_cast<uint32_t>(indices.size()), false};
}

void ExpectCacheMatchesFullRaster(const raster::CommandCache& cache,
                                  const std::vector<CustomOverlay::DrawVertex>& vertices,
                                  const std::vector<uint16_t>& indices,
                                  const std::vector<CustomOverlay::DrawCommand>& commands,
                                  const raster::Target& target) {
    std::vector<uint32_t> expected;
    ASSERT_TRUE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, target, expected));
    EXPECT_EQ(cache.composed, expected);
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

TEST(OverlayCpuRasterTest, FractionalQuadEdgesMatchPixelCentreCoverage) {
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 1.7f, 1.7f, 3.0f, 3.0f, 0xFFFFFFFFu);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, false}};

    std::vector<uint32_t> out;
    ASSERT_TRUE(raster::Rasterize(vertices, indices, commands, raster::FontAtlasView{}, MakeTarget(0, 0, 7, 7), out));
    EXPECT_EQ(out[1 * 7 + 1], 0u);
    EXPECT_EQ(out[2 * 7 + 2], 0xFFFFFFFFu);
    EXPECT_EQ(out[4 * 7 + 4], 0xFFFFFFFFu);
    EXPECT_EQ(out[5 * 7 + 5], 0u);
}

TEST(OverlayCpuRasterTest, TexturedQuadPremultipliesByAtlasAndVertexAlpha) {
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 0.0f, 0.0f, 2.0f, 2.0f, 0x800000FFu);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, true}};
    const uint8_t whiteAtlas[] = {255, 255, 255, 255};
    const raster::FontAtlasView atlas{whiteAtlas, 1, 1};

    std::vector<uint32_t> out;
    ASSERT_TRUE(raster::Rasterize(vertices, indices, commands, atlas, MakeTarget(0, 0, 2, 2), out));
    const uint32_t pixel = out[0];
    EXPECT_EQ((pixel >> 24) & 0xFFu, 0x80u);
    EXPECT_GE((pixel >> 16) & 0xFFu, 0x7Du);
    EXPECT_LE((pixel >> 16) & 0xFFu, 0x83u);
    EXPECT_EQ(pixel & 0xFFFFu, 0u);
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

TEST(OverlayCpuRasterTest, PrimitiveCacheRepaintsOnlyTheMovingQuadInsideAMergedCommand) {
    const auto target = MakeTarget(0, 0, 20, 10);
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 0.0f, 0.0f, 8.0f, 8.0f, 0x800000FFu);
    AppendQuad(vertices, indices, 12.0f, 1.0f, 4.0f, 2.0f, 0xFFFFFFFFu);
    std::vector<CustomOverlay::DrawCommand> commands{MakeMergedCommand(vertices, indices)};

    raster::CommandCache cache;
    raster::RasterStats firstStats;
    raster::PixelRect firstDirty;
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target,
                                           firstStats, firstDirty));
    EXPECT_EQ(firstStats.fullRasters, 1u);

    for (size_t i = 4; i < 8; ++i)
        vertices[i].y += 2.0f;
    raster::RasterStats secondStats;
    raster::PixelRect secondDirty;
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target,
                                           secondStats, secondDirty));

    EXPECT_EQ(secondStats.fullRasters, 0u);
    EXPECT_EQ(secondStats.dirtyRasters, 1u);
    EXPECT_GT(secondDirty.left, 8);
    EXPECT_EQ(secondStats.primitiveReuses, 1u);
    ExpectCacheMatchesFullRaster(cache, vertices, indices, commands, target);
}

TEST(OverlayCpuRasterTest, PrimitiveCacheClearsPixelsVacatedByARemovedPrimitive) {
    const auto target = MakeTarget(0, 0, 24, 8);
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 1.0f, 1.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    AppendQuad(vertices, indices, 9.0f, 1.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    AppendQuad(vertices, indices, 17.0f, 1.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    std::vector<CustomOverlay::DrawCommand> commands{MakeMergedCommand(vertices, indices)};

    raster::CommandCache cache;
    raster::RasterStats stats;
    raster::PixelRect dirty;
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));

    vertices.clear();
    indices.clear();
    AppendQuad(vertices, indices, 1.0f, 1.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    AppendQuad(vertices, indices, 17.0f, 1.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    commands[0] = MakeMergedCommand(vertices, indices);
    stats = {};
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));

    EXPECT_EQ(stats.dirtyRasters, 1u);
    EXPECT_GE(stats.primitiveReuses, 2u);
    EXPECT_EQ(cache.composed[2 * target.width + 10], 0u);
    ExpectCacheMatchesFullRaster(cache, vertices, indices, commands, target);
}

TEST(OverlayCpuRasterTest, PrimitiveCacheNeverBlendsOntoAnObsoleteCachedPrefix) {
    const auto target = MakeTarget(0, 0, 16, 8);
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 1.0f, 1.0f, 5.0f, 5.0f, 0x800000FFu);
    AppendQuad(vertices, indices, 9.0f, 1.0f, 5.0f, 5.0f, 0x8000FF00u);
    std::vector<CustomOverlay::DrawCommand> commands{{0, 4, 0, 6, false}, {4, 4, 6, 6, false}};

    raster::CommandCache cache;
    raster::RasterStats stats;
    raster::PixelRect dirty;
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));

    for (size_t i = 4; i < 8; ++i)
        vertices[i].y += 1.0f;
    stats = {};
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));

    for (size_t i = 0; i < 4; ++i)
        vertices[i].x += 1.0f;
    stats = {};
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));
    EXPECT_EQ(stats.dirtyRasters, 1u);
    ExpectCacheMatchesFullRaster(cache, vertices, indices, commands, target);
}

TEST(OverlayCpuRasterTest, PrimitiveCacheDoesNoRasterWorkForAnIdenticalFrame) {
    const auto target = MakeTarget(0, 0, 8, 8);
    std::vector<CustomOverlay::DrawVertex> vertices;
    std::vector<uint16_t> indices;
    AppendQuad(vertices, indices, 1.0f, 1.0f, 4.0f, 4.0f, 0xFFFFFFFFu);
    std::vector<CustomOverlay::DrawCommand> commands{MakeMergedCommand(vertices, indices)};
    raster::CommandCache cache;
    raster::RasterStats stats;
    raster::PixelRect dirty;
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));

    stats = {};
    ASSERT_TRUE(raster::UpdateCommandCache(cache, vertices, indices, commands, raster::FontAtlasView{}, target, stats,
                                           dirty));
    EXPECT_EQ(stats.spriteReuses, 1u);
    EXPECT_EQ(stats.fullRasters, 0u);
    EXPECT_EQ(stats.dirtyRasters, 0u);
    EXPECT_TRUE(dirty.IsEmpty());
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
