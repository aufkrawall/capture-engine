#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../mediaengine/cursor_geometry.h"
#include "../mediaengine/encode_geometry_policy.h"
#include "source_fragment_reader.h"

namespace geo = ce::encode_geometry;
using Action = geo::SourceChangeAction;

TEST(EncodeGeometryPolicyTest, CommittedOutputNeverReinitializes) {
    // Before the file is open, a contract change re-initializes (nothing committed).
    EXPECT_EQ(geo::ClassifySourceChange(false, true, 1920, 1080, 1920, 1080), Action::kReinitialize);
    EXPECT_EQ(geo::ClassifySourceChange(false, false, 2560, 1440, 1920, 1080), Action::kNone);
    // After commit: a contract change finalizes (re-opening the output truncated it),
    // a size change is fitted, an unchanged source passes.
    EXPECT_EQ(geo::ClassifySourceChange(true, true, 1920, 1080, 1920, 1080), Action::kFinalizeAndStop);
    EXPECT_EQ(geo::ClassifySourceChange(true, true, 1280, 720, 1920, 1080), Action::kFinalizeAndStop);
    EXPECT_EQ(geo::ClassifySourceChange(true, false, 1280, 720, 1920, 1080), Action::kFitIntoLockedFrame);
    EXPECT_EQ(geo::ClassifySourceChange(true, false, 1920, 1080, 1920, 1080), Action::kNone);
    EXPECT_EQ(geo::ClassifySourceChange(true, false, 1280, 720, 0, 0), Action::kNone);
}

TEST(EncodeGeometryPolicyTest, FitPreservesAspectAndCentres) {
    // Same aspect: fills the frame.
    geo::Rect fit = geo::FitSourceIntoFrame(1280, 720, 1920, 1080);
    EXPECT_EQ(fit.x, 0);
    EXPECT_EQ(fit.y, 0);
    EXPECT_EQ(fit.width, 1920);
    EXPECT_EQ(fit.height, 1080);
    // 4:3 into 16:9: pillarboxed.
    fit = geo::FitSourceIntoFrame(1024, 768, 1920, 1080);
    EXPECT_EQ(fit.width, 1440);
    EXPECT_EQ(fit.height, 1080);
    EXPECT_EQ(fit.x, 240);
    EXPECT_EQ(fit.y, 0);
    // Ultrawide into 16:9: letterboxed.
    fit = geo::FitSourceIntoFrame(3440, 1440, 1920, 1080);
    EXPECT_EQ(fit.width, 1920);
    EXPECT_EQ(fit.height, 804);
    EXPECT_EQ(fit.y, 138);
    // A tiny window never produces an empty rectangle.
    fit = geo::FitSourceIntoFrame(1, 10000, 1920, 1080);
    EXPECT_GE(fit.width, 1);
    EXPECT_EQ(fit.height, 1080);
    // Degenerate input yields nothing to draw.
    fit = geo::FitSourceIntoFrame(0, 720, 1920, 1080);
    EXPECT_EQ(fit.width, 0);
}

TEST(EncodeGeometryPolicyTest, CursorLandsInsideTheFittedRectangle) {
    ce::cursor::CaptureState state;
    state.captureLeft = 100;
    state.captureTop = 50;
    state.captureWidth = 1024;
    state.captureHeight = 768;
    const geo::Rect fit = geo::FitSourceIntoFrame(1024, 768, 1920, 1080);
    const ce::cursor::CaptureState adjusted = geo::AdjustCursorForFit(state, 1024, 768, 1920, 1080, fit);

    // The capture area's top-left and bottom-right corners map onto the fitted rectangle.
    ce::cursor_geometry::Rect topLeft;
    ASSERT_TRUE(ce::cursor_geometry::MapScreenCursorToFrame(
        100, 50, 0, 0, 32, 32, adjusted.captureLeft, adjusted.captureTop, static_cast<int>(adjusted.captureWidth),
        static_cast<int>(adjusted.captureHeight), 1920, 1080, &topLeft));
    EXPECT_NEAR(topLeft.left, fit.x, 1);
    EXPECT_NEAR(topLeft.top, fit.y, 1);
    ce::cursor_geometry::Rect bottomRight;
    ASSERT_TRUE(ce::cursor_geometry::MapScreenCursorToFrame(
        100 + 1024, 50 + 768, 0, 0, 32, 32, adjusted.captureLeft, adjusted.captureTop,
        static_cast<int>(adjusted.captureWidth), static_cast<int>(adjusted.captureHeight), 1920, 1080, &bottomRight));
    EXPECT_NEAR(bottomRight.left, fit.x + fit.width, 2);
    EXPECT_NEAR(bottomRight.top, fit.y + fit.height, 2);
    // Cursor size scales like the source did (1080/768).
    EXPECT_NEAR(topLeft.right - topLeft.left, 32 * 1080 / 768, 1);
}

TEST(EncodeGeometryPolicyTest, EncoderRoutesSourceChangesThroughThePolicy) {
    const auto root = std::filesystem::current_path();
    const std::string encode = ce::test_source::ReadLogicalSource(root / "mediaengine/video_encoder_encode.cpp");
    const std::string framegrab = ce::test_source::ReadLogicalSource(root / "mediaengine/video_encoder_framegrab.cpp");
    ASSERT_FALSE(encode.empty());
    ASSERT_FALSE(framegrab.empty());
    for (const std::string* source : {&encode, &framegrab}) {
        EXPECT_NE(source->find("ClassifySourceChange("), std::string::npos);
        EXPECT_NE(source->find("RequestStopForSourceContractChange("), std::string::npos);
        EXPECT_NE(source->find("FitSourceToLockedGeometry("), std::string::npos);
    }
}
