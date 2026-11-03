#include <gtest/gtest.h>

#include <cstring>

#include "hook/overlay/custom_font.h"

// Compiled into the test binary directly: the unit tests link common/ and mediaengine/, not the hook DLL.
// NOLINTNEXTLINE(bugprone-suspicious-include) - deliberate, see above
#include "hook/overlay/custom_font.cpp"

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

// Solid shapes sample this block, so it must be fully white around the published coordinates and a
// linear filter on any glyph must not reach it. Both atlas sizes (scale above 1.5 doubles it).
TEST(CustomFontAtlasCache, TheWhiteBlockIsOpaqueAndNoGlyphComesNearIt) {
    for (const float scale : {1.0f, 2.0f, 4.0f}) {
        FontAtlas atlas;
        ASSERT_TRUE(atlas.Initialize("Arial", 16, scale));
        const int width = atlas.GetTextureWidth();
        const int height = atlas.GetTextureHeight();
        const int blockX = static_cast<int>(atlas.GetWhiteU() * static_cast<float>(width) + 0.5f);
        const int blockY = static_cast<int>(atlas.GetWhiteV() * static_cast<float>(height) + 0.5f);
        EXPECT_EQ(blockX, 2);
        EXPECT_EQ(blockY, height - 2);
        for (int y = height - 4; y < height; ++y) {
            for (int x = 0; x < 4; ++x) {
                const uint8_t* texel = atlas.GetTextureData() + (static_cast<size_t>(y) * width + x) * 4u;
                EXPECT_EQ(std::memcmp(texel, "\xFF\xFF\xFF\xFF", 4), 0) << x << "," << y << " scale " << scale;
            }
        }
        for (char c = 32; c < 127; ++c) {
            const auto* glyph = atlas.GetGlyph(c);
            EXPECT_GT(glyph->width, 0) << "the block must not push out a glyph: '" << c << "' scale " << scale;
            EXPECT_LE(glyph->y + glyph->height + 2, height - 4) << "'" << c << "' scale " << scale;
        }
    }
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
