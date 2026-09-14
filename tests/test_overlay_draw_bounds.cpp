/**
 * Draw-bounds tests for the overlay renderer.
 *
 * The legacy DirectDraw/DX6/DX7 composite stages the game's own pixels through
 * the CPU, so it moves only the rectangle the overlay's geometry occupies. A
 * bounds result that is wrong by construction - empty, stale after a cached
 * frame, or covering the whole frame - turns that composite back into a
 * full-surface round trip or clips the overlay.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../hook/common/custom_overlay.h"

using namespace CustomOverlay;

namespace {

class BoundsMockBackend : public RendererBackend {
public:
    bool Initialize(int, int, const uint8_t*) override {
        return true;
    }
    void Shutdown() override {}
    void Render(const std::vector<DrawVertex>&, const std::vector<uint16_t>&, const std::vector<DrawCommand>&, int,
                int) override {
        renderCallCount++;
    }

    int renderCallCount = 0;
};

class OverlayDrawBoundsTest : public ::testing::Test {
protected:
    BoundsMockBackend backend;
    Renderer renderer;

    void SetUp() override {
        ASSERT_TRUE(renderer.Initialize(&backend, 1.0f));
    }
};

}  // namespace

TEST_F(OverlayDrawBoundsTest, DrawBoundsAreEmptyBeforeAnythingIsDrawn) {
    float minX = -1.0f;
    float minY = -1.0f;
    float maxX = -1.0f;
    float maxY = -1.0f;
    EXPECT_FALSE(renderer.GetDrawBounds(minX, minY, maxX, maxY));
}

TEST_F(OverlayDrawBoundsTest, DrawBoundsEncloseTheGeometryThatWasEmitted) {
    renderer.BeginFrame(1920, 1080);
    renderer.DrawRectFilled(40.0f, 24.0f, 200.0f, 60.0f, 0xFF204080u);
    renderer.EndFrame();

    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    ASSERT_TRUE(renderer.GetDrawBounds(minX, minY, maxX, maxY));
    EXPECT_FLOAT_EQ(minX, 40.0f);
    EXPECT_FLOAT_EQ(minY, 24.0f);
    EXPECT_FLOAT_EQ(maxX, 240.0f);
    EXPECT_FLOAT_EQ(maxY, 84.0f);
}

TEST_F(OverlayDrawBoundsTest, DrawBoundsCoverEveryEmittedShape) {
    renderer.BeginFrame(1920, 1080);
    renderer.DrawRectFilled(100.0f, 100.0f, 10.0f, 10.0f, 0xFFFFFFFFu);
    renderer.DrawRectFilled(700.0f, 400.0f, 20.0f, 30.0f, 0xFFFFFFFFu);
    renderer.EndFrame();

    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    ASSERT_TRUE(renderer.GetDrawBounds(minX, minY, maxX, maxY));
    EXPECT_FLOAT_EQ(minX, 100.0f);
    EXPECT_FLOAT_EQ(minY, 100.0f);
    EXPECT_FLOAT_EQ(maxX, 720.0f);
    EXPECT_FLOAT_EQ(maxY, 430.0f);
}

TEST_F(OverlayDrawBoundsTest, DrawBoundsSurviveTheCachedFramePath) {
    // The DirectDraw composite stages its region from these bounds on every
    // present, including the frames that re-submit cached geometry instead of
    // rebuilding it. Losing them there would stage nothing and skip the
    // overlay for that frame.
    renderer.BeginFrame(1920, 1080);
    renderer.DrawRectFilled(15.0f, 15.0f, 465.0f, 245.0f, 0xFF000000u);
    renderer.EndFrame();

    float builtMinX = 0.0f;
    float builtMinY = 0.0f;
    float builtMaxX = 0.0f;
    float builtMaxY = 0.0f;
    ASSERT_TRUE(renderer.GetDrawBounds(builtMinX, builtMinY, builtMaxX, builtMaxY));

    ASSERT_TRUE(renderer.RenderCachedFrame(1920, 1080));

    float cachedMinX = 0.0f;
    float cachedMinY = 0.0f;
    float cachedMaxX = 0.0f;
    float cachedMaxY = 0.0f;
    ASSERT_TRUE(renderer.GetDrawBounds(cachedMinX, cachedMinY, cachedMaxX, cachedMaxY));
    EXPECT_FLOAT_EQ(cachedMinX, builtMinX);
    EXPECT_FLOAT_EQ(cachedMinY, builtMinY);
    EXPECT_FLOAT_EQ(cachedMaxX, builtMaxX);
    EXPECT_FLOAT_EQ(cachedMaxY, builtMaxY);
}

TEST_F(OverlayDrawBoundsTest, DrawBoundsStayFarSmallerThanA4KFrame) {
    // The point of staging a region: an overlay composite must never move the
    // frame. A default-shaped overlay covers well under a twentieth of 4K.
    renderer.BeginFrame(3840, 2160);
    renderer.DrawRectFilled(15.0f, 15.0f, 465.0f, 245.0f, 0xE0282828u);
    renderer.DrawText(20.0f, 20.0f, "FPS 144.0", 0xFF00FF00u);
    renderer.EndFrame();

    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    ASSERT_TRUE(renderer.GetDrawBounds(minX, minY, maxX, maxY));
    const double overlayPixels = static_cast<double>(maxX - minX) * static_cast<double>(maxY - minY);
    EXPECT_LT(overlayPixels * 20.0, 3840.0 * 2160.0);
}
