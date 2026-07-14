#include <gtest/gtest.h>

#include "../mediaengine/cursor_geometry.h"
#include "../common/cursor_capture_state.h"

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

TEST(CursorGeometryTest, MapsNegativeMonitorOriginAndHotspotIntoFrame) {
    cg::Rect result;
    ASSERT_TRUE(cg::MapScreenCursorToFrame(-1860, 120, 8, 12, 48, 48, -1920, 0, 1920, 1080, 1920, 1080,
                                           &result));

    EXPECT_EQ(result.left, 52);
    EXPECT_EQ(result.top, 108);
    EXPECT_EQ(result.right, 100);
    EXPECT_EQ(result.bottom, 156);
}

TEST(CursorGeometryTest, MapsWindowClientCoordinatesToScaledSwapChain) {
    cg::Rect result;
    ASSERT_TRUE(cg::MapScreenCursorToFrame(600, 350, 10, 10, 60, 60, 100, 50, 1000, 600, 2000, 1200, &result));

    EXPECT_EQ(result.left, 980);
    EXPECT_EQ(result.top, 580);
    EXPECT_EQ(result.right, 1100);
    EXPECT_EQ(result.bottom, 700);
}

TEST(CursorGeometryTest, HotspotAndShapeTopLeftCoordinatesMapToTheSameRectangle) {
    constexpr int hotspotX = 17;
    constexpr int hotspotY = 15;
    cg::Rect hotspotResult;
    cg::Rect topLeftResult;

    ASSERT_TRUE(cg::MapScreenCursorToFrame(117, 215, cg::ResolveHotspotForPosition(hotspotX, false),
                                           cg::ResolveHotspotForPosition(hotspotY, false), 48, 48, 0, 0, 1920, 1080,
                                           1920, 1080, &hotspotResult));
    ASSERT_TRUE(cg::MapScreenCursorToFrame(100, 200, cg::ResolveHotspotForPosition(hotspotX, true),
                                           cg::ResolveHotspotForPosition(hotspotY, true), 48, 48, 0, 0, 1920, 1080,
                                           1920, 1080, &topLeftResult));

    EXPECT_EQ(hotspotResult.left, 100);
    EXPECT_EQ(hotspotResult.top, 200);
    EXPECT_EQ(topLeftResult.left, hotspotResult.left);
    EXPECT_EQ(topLeftResult.top, hotspotResult.top);
    EXPECT_EQ(topLeftResult.right, hotspotResult.right);
    EXPECT_EQ(topLeftResult.bottom, hotspotResult.bottom);
}

TEST(CursorCaptureStateTest, AppliesAuthoritativeShapeTopLeftPointerObservation) {
    ce::cursor::CaptureState state;
    state.handle = 1;
    state.flags = ce::cursor::kStateValid | ce::cursor::kStateVisible |
                  ce::cursor::kStateHandleVisibilityFallback;
    state.associationQpc = 100;
    state.observedQpc = 110;
    state.screenX = 117;
    state.screenY = 215;

    ce::cursor::SourcePointerObservation observation;
    observation.valid = true;
    observation.visible = true;
    observation.positionValid = true;
    observation.positionIsShapeTopLeft = true;
    observation.updateQpc = 200;
    observation.screenX = 100;
    observation.screenY = 200;
    ce::cursor::ApplySourcePointerObservation(&state, observation);

    EXPECT_TRUE(state.IsVisible());
    EXPECT_TRUE(state.PositionIsShapeTopLeft());
    EXPECT_EQ(state.screenX, 100);
    EXPECT_EQ(state.screenY, 200);
    EXPECT_EQ(state.associationQpc, 200);
    EXPECT_EQ(state.observedQpc, 200);
    EXPECT_EQ(state.flags & ce::cursor::kStateHandleVisibilityFallback, 0u);
}

TEST(CursorCaptureStateTest, AuthoritativeHiddenPointerClearsHandleVisibilityFallback) {
    ce::cursor::CaptureState state;
    state.handle = 1;
    state.flags = ce::cursor::kStateValid | ce::cursor::kStateVisible |
                  ce::cursor::kStateHandleVisibilityFallback | ce::cursor::kStatePositionIsShapeTopLeft;

    ce::cursor::SourcePointerObservation observation;
    observation.valid = true;
    observation.updateQpc = 300;
    ce::cursor::ApplySourcePointerObservation(&state, observation);

    EXPECT_FALSE(state.IsVisible());
    EXPECT_FALSE(state.PositionIsShapeTopLeft());
    EXPECT_EQ(state.flags & ce::cursor::kStateHandleVisibilityFallback, 0u);
}

TEST(CursorCaptureStateTest, EmbeddedPointerSuppressesEncoderComposition) {
    ce::cursor::CaptureState state;
    state.handle = 1;
    state.flags = ce::cursor::kStateValid | ce::cursor::kStateVisible;

    ce::cursor::SourcePointerObservation observation;
    observation.valid = true;
    observation.embedded = true;
    observation.updateQpc = 400;
    ce::cursor::ApplySourcePointerObservation(&state, observation);

    EXPECT_FALSE(state.IsVisible());
    EXPECT_NE(state.flags & ce::cursor::kStateSuppressed, 0u);
}

TEST(CursorTimelineTest, SelectsNewestStateAtOrBeforeContentTime) {
    ce::cursor::Timeline timeline(4);
    ce::cursor::CaptureState first;
    first.flags = ce::cursor::kStateValid;
    first.associationQpc = 100;
    first.screenX = 10;
    ce::cursor::CaptureState second = first;
    second.associationQpc = 200;
    second.screenX = 20;

    timeline.Publish(second);
    timeline.Publish(first);

    ce::cursor::CaptureState selected;
    EXPECT_FALSE(timeline.SelectAtOrBefore(99, &selected));
    ASSERT_TRUE(timeline.SelectAtOrBefore(150, &selected));
    EXPECT_EQ(selected.screenX, 10);
    ASSERT_TRUE(timeline.SelectAtOrBefore(200, &selected));
    EXPECT_EQ(selected.screenX, 20);
}

TEST(CursorTimelineTest, DropsOldestSamplesAtCapacity) {
    ce::cursor::Timeline timeline(2);
    for (int64_t qpc : {100, 200, 300}) {
        ce::cursor::CaptureState state;
        state.flags = ce::cursor::kStateValid;
        state.associationQpc = qpc;
        timeline.Publish(state);
    }

    ce::cursor::CaptureState selected;
    EXPECT_FALSE(timeline.SelectAtOrBefore(100, &selected));
    ASSERT_TRUE(timeline.SelectAtOrBefore(250, &selected));
    EXPECT_EQ(selected.associationQpc, 200);
}
