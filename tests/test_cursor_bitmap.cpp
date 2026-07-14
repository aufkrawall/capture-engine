#include <gtest/gtest.h>

#include "../mediaengine/cursor_bitmap_utils.h"
#include "../mediaengine/cursor_renderer.h"

namespace cb = ce::cursor_bitmap;

TEST(CursorBitmapTest, ReconstructsTransparentPixel) {
    const cb::Bgra8 result = cb::ReconstructStraightAlpha({0, 0, 0, 255}, {255, 255, 255, 255});
    EXPECT_EQ(result.b, 0);
    EXPECT_EQ(result.g, 0);
    EXPECT_EQ(result.r, 0);
    EXPECT_EQ(result.a, 0);
}

TEST(CursorBitmapTest, ReconstructsOpaqueColor) {
    const cb::Bgra8 result = cb::ReconstructStraightAlpha({24, 80, 220, 255}, {24, 80, 220, 255});
    EXPECT_EQ(result.b, 24);
    EXPECT_EQ(result.g, 80);
    EXPECT_EQ(result.r, 220);
    EXPECT_EQ(result.a, 255);
}

TEST(CursorBitmapTest, ConvertsPremultipliedAntialiasedWhiteToStraightAlpha) {
    const cb::Bgra8 result = cb::ReconstructStraightAlpha({128, 128, 128, 255}, {255, 255, 255, 255});
    EXPECT_EQ(result.b, 255);
    EXPECT_EQ(result.g, 255);
    EXPECT_EQ(result.r, 255);
    EXPECT_EQ(result.a, 128);
}

TEST(CursorBitmapTest, MedianBackgroundDifferenceIgnoresOneChannelRoundingError) {
    const cb::Bgra8 result = cb::ReconstructStraightAlpha({128, 128, 128, 255}, {255, 254, 255, 255});
    EXPECT_EQ(result.a, 128);
    EXPECT_EQ(result.b, 255);
    EXPECT_EQ(result.g, 255);
    EXPECT_EQ(result.r, 255);
}

TEST(CursorBitmapTest, MonochromeInvertPixelGetsStableOpaqueApproximation) {
    const cb::Bgra8 result = cb::ReconstructStraightAlpha({255, 255, 255, 255}, {0, 0, 0, 255});
    EXPECT_EQ(result.b, 255);
    EXPECT_EQ(result.g, 255);
    EXPECT_EQ(result.r, 255);
    EXPECT_EQ(result.a, 255);
}

TEST(CursorBitmapTest, LoadsSystemCursorThroughCanonicalDrawIconRepresentation) {
    HCURSOR cursor = LoadCursorA(nullptr, IDC_ARROW);
    ASSERT_NE(cursor, nullptr);

    CursorRenderer renderer;
    CursorBitmapData bitmap;
    ASSERT_TRUE(renderer.LoadCursorBitmap(cursor, 0, 0, &bitmap));
    ASSERT_NE(bitmap.pixels, nullptr);
    ASSERT_GT(bitmap.width, 0u);
    ASSERT_GT(bitmap.height, 0u);

    bool hasVisiblePixel = false;
    bool hasTransparentPixel = false;
    for (size_t i = 0; i < static_cast<size_t>(bitmap.width) * bitmap.height; ++i) {
        hasVisiblePixel |= bitmap.pixels[i * 4 + 3] != 0;
        hasTransparentPixel |= bitmap.pixels[i * 4 + 3] == 0;
    }
    EXPECT_TRUE(hasVisiblePixel);
    EXPECT_TRUE(hasTransparentPixel);
}
