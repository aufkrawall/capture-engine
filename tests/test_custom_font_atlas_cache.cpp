#include <gtest/gtest.h>

#include <cstring>

#include "../hook/common/custom_font.h"

// Compiled into the test binary directly: the unit tests link common/ and mediaengine/, not the hook DLL.
#include "../hook/common/custom_font.cpp"

// GTA session 20260925_225006: at FSR FG start CE built two overlay renderers on AMD's presenter thread, each
// re-rasterizing the same GDI font atlas (~4 ms). The finished atlas is now shared by (font, size, scale).

namespace {

using CustomOverlay::FontAtlas;

void ExpectSameAtlas(const FontAtlas& a, const FontAtlas& b) {
    ASSERT_EQ(a.GetTextureWidth(), b.GetTextureWidth());
    ASSERT_EQ(a.GetTextureHeight(), b.GetTextureHeight());
    EXPECT_EQ(a.GetLineHeight(), b.GetLineHeight());
    const size_t bytes = static_cast<size_t>(a.GetTextureWidth()) * static_cast<size_t>(a.GetTextureHeight()) * 4u;
    EXPECT_EQ(std::memcmp(a.GetTextureData(), b.GetTextureData(), bytes), 0);
    for (char c = 32; c < 127; ++c) {
        const auto* ga = a.GetGlyph(c);
        const auto* gb = b.GetGlyph(c);
        EXPECT_EQ(ga->x, gb->x);
        EXPECT_EQ(ga->y, gb->y);
        EXPECT_EQ(ga->width, gb->width);
        EXPECT_EQ(ga->xAdvance, gb->xAdvance);
        EXPECT_EQ(a.GetGlyphSpans(c).size(), b.GetGlyphSpans(c).size());
    }
}

TEST(CustomFontAtlasCache, SecondRendererReusesTheRasterizedAtlas) {
    FontAtlas first;
    ASSERT_TRUE(first.Initialize("Arial", 14, 1.25f));
    const uint32_t afterFirst = FontAtlas::RasterizedAtlasCount();

    FontAtlas second;
    ASSERT_TRUE(second.Initialize("Arial", 14, 1.25f));
    EXPECT_EQ(FontAtlas::RasterizedAtlasCount(), afterFirst) << "same key must not rasterize again";
    ExpectSameAtlas(first, second);
}

TEST(CustomFontAtlasCache, DifferentScaleIsADifferentAtlas) {
    FontAtlas base;
    ASSERT_TRUE(base.Initialize("Arial", 14, 1.0f));
    const uint32_t before = FontAtlas::RasterizedAtlasCount();
    FontAtlas scaled;
    ASSERT_TRUE(scaled.Initialize("Arial", 14, 2.0f));
    EXPECT_EQ(FontAtlas::RasterizedAtlasCount(), before + 1);
    EXPECT_GT(scaled.GetLineHeight(), base.GetLineHeight());
}

TEST(CustomFontAtlasCache, ShutdownOfOneAtlasLeavesTheCachedCopyIntact) {
    FontAtlas a;
    ASSERT_TRUE(a.Initialize("Arial", 15, 1.0f));
    FontAtlas reference;
    ASSERT_TRUE(reference.Initialize("Arial", 15, 1.0f));
    a.Shutdown();
    FontAtlas b;
    ASSERT_TRUE(b.Initialize("Arial", 15, 1.0f));
    ExpectSameAtlas(reference, b);
}

}  // namespace
