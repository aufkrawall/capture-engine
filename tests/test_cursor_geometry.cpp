#include <gtest/gtest.h>

#include "../mediaengine/cursor_geometry.h"

namespace cg = ce::cursor_geometry;

TEST(CursorGeometryTest, FullyVisibleCursorKeepsFullSourceAndDestination) {
    cg::ClippedRects result;
    ASSERT_TRUE(cg::ComputeClippedRects({10, 20, 42, 52}, 32, 32, 1920, 1080, &result));

    EXPECT_EQ(result.destination.left, 10);
    EXPECT_EQ(result.destination.top, 20);
    EXPECT_EQ(result.destination.right, 42);
    EXPECT_EQ(result.destination.bottom, 52);
    EXPECT_EQ(result.source.left, 0);
    EXPECT_EQ(result.source.top, 0);
    EXPECT_EQ(result.source.right, 32);
    EXPECT_EQ(result.source.bottom, 32);
}

TEST(CursorGeometryTest, LeftAndTopClippingCropsSourceProportionally) {
    cg::ClippedRects result;
    ASSERT_TRUE(cg::ComputeClippedRects({-8, -4, 24, 28}, 64, 64, 1920, 1080, &result));

    EXPECT_EQ(result.destination.left, 0);
    EXPECT_EQ(result.destination.top, 0);
    EXPECT_EQ(result.destination.right, 24);
    EXPECT_EQ(result.destination.bottom, 28);
    EXPECT_EQ(result.source.left, 16);
    EXPECT_EQ(result.source.top, 8);
    EXPECT_EQ(result.source.right, 64);
    EXPECT_EQ(result.source.bottom, 64);
}

TEST(CursorGeometryTest, RightAndBottomClippingCropsScaledSourceWithoutSquashing) {
    cg::ClippedRects result;
    ASSERT_TRUE(cg::ComputeClippedRects({90, 70, 110, 90}, 32, 32, 100, 80, &result));

    EXPECT_EQ(result.destination.left, 90);
    EXPECT_EQ(result.destination.top, 70);
    EXPECT_EQ(result.destination.right, 100);
    EXPECT_EQ(result.destination.bottom, 80);
    EXPECT_EQ(result.source.left, 0);
    EXPECT_EQ(result.source.top, 0);
    EXPECT_EQ(result.source.right, 16);
    EXPECT_EQ(result.source.bottom, 16);
}

TEST(CursorGeometryTest, FractionalScalingUsesConservativeSourceEdges) {
    cg::ClippedRects result;
    ASSERT_TRUE(cg::ComputeClippedRects({-1, 0, 4, 5}, 7, 7, 4, 5, &result));

    EXPECT_EQ(result.source.left, 1);
    EXPECT_EQ(result.source.right, 7);
}

TEST(CursorGeometryTest, DestinationScalingRoundsOutwardIncludingNegativeEdges) {
    cg::Rect result;
    ASSERT_TRUE(cg::ScaleDestinationRect({-3, -1, 5, 7}, 100, 50, 200, 100, &result));

    EXPECT_EQ(result.left, -2);
    EXPECT_EQ(result.top, -1);
    EXPECT_EQ(result.right, 3);
    EXPECT_EQ(result.bottom, 4);
}

TEST(CursorGeometryTest, RejectsEmptyOrFullyInvisibleGeometry) {
    cg::ClippedRects result;
    EXPECT_FALSE(cg::ComputeClippedRects({10, 10, 10, 20}, 32, 32, 100, 100, &result));
    EXPECT_FALSE(cg::ComputeClippedRects({-20, 10, -1, 20}, 32, 32, 100, 100, &result));
    EXPECT_FALSE(cg::ComputeClippedRects({0, 0, 20, 20}, 0, 32, 100, 100, &result));
    EXPECT_FALSE(cg::ComputeClippedRects({0, 0, 20, 20}, 32, 32, 100, 100, nullptr));
}
