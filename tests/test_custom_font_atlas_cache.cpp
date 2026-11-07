#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "hook/overlay/custom_font.h"
#include "hook/overlay/overlay_font_policy.h"

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
// linear filter on any glyph must not reach it, whatever size the atlas chose.
TEST(CustomFontAtlasCache, TheWhiteBlockIsOpaqueAndNoGlyphComesNearIt) {
    for (const float scale : {1.0f, 2.0f, 4.0f}) {
        FontAtlas atlas;
        ASSERT_TRUE(atlas.Initialize("Arial", 16, scale));
        const int width = atlas.GetTextureWidth();
        const int height = atlas.GetTextureHeight();
        const int blockX = static_cast<int>(std::lround(atlas.GetWhiteU() * static_cast<float>(width)));
        const int blockY = static_cast<int>(std::lround(atlas.GetWhiteV() * static_cast<float>(height)));
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

using CustomOverlay::TextSize;

// GDI cannot select OpenType tabular figures, so without this a right-aligned column twitches with
// every value in any font with proportional digits. Segoe UI Semibold's run from 8 to 12 px at 21 px.
TEST(CustomFontAtlas, DigitsShareOneAdvanceInEverySize) {
    for (const char* face : {"Arial", "Segoe UI Semibold", "Bahnschrift SemiBold"}) {
        FontAtlas atlas;
        if (!atlas.Initialize(face, 14, 1.5f)) {
            ASSERT_STRNE(face, "Arial") << "the test needs at least Arial";
            continue;
        }
        for (const TextSize size : {TextSize::Base, TextSize::Small, TextSize::Hero}) {
            const uint16_t advance = atlas.GetGlyph('0', size)->xAdvance;
            EXPECT_GT(advance, 0) << face;
            for (char digit = '1'; digit <= '9'; ++digit) {
                const auto* glyph = atlas.GetGlyph(digit, size);
                EXPECT_EQ(glyph->xAdvance, advance) << face << " '" << digit << "'";
                // The digit stays inside its cell, not shoved to one side of it.
                EXPECT_GE(glyph->xOffset, -2) << face << " '" << digit << "'";
                EXPECT_LE(glyph->xOffset + static_cast<int>(glyph->width), advance + 2) << face << " '" << digit << "'";
            }
        }
    }
}

TEST(CustomFontAtlas, SizesAreRasterizedSeparatelyAndHeroHoldsOnlyWhatItDraws) {
    FontAtlas atlas;
    ASSERT_TRUE(atlas.Initialize("Arial", 14, 1.5f));
    const auto& base = atlas.GetMetrics(TextSize::Base);
    const auto& small = atlas.GetMetrics(TextSize::Small);
    const auto& hero = atlas.GetMetrics(TextSize::Hero);
    EXPECT_EQ(base.pixelSize, 21);
    EXPECT_LT(small.pixelSize, base.pixelSize);
    EXPECT_GT(hero.pixelSize, base.pixelSize);
    EXPECT_GT(hero.digitHeight, base.digitHeight);
    EXPECT_GT(base.digitHeight, small.digitHeight);
    EXPECT_GT(atlas.GetGlyph('7', TextSize::Hero)->width, atlas.GetGlyph('7', TextSize::Base)->width);
    EXPECT_FALSE(atlas.GetGlyphSpans('7', TextSize::Hero).empty()) << "solid-text backends draw these spans";
    EXPECT_FALSE(atlas.GetGlyphSpans('7', TextSize::Small).empty());
    EXPECT_EQ(atlas.GetGlyph('Q', TextSize::Hero)->width, 0) << "no letters at hero size";
    EXPECT_GT(atlas.GetGlyph('Q', TextSize::Small)->width, 0);
}

TEST(CustomFontAtlas, DegreeSignHasItsOwnGlyphInTheTextSizes) {
    FontAtlas atlas;
    ASSERT_TRUE(atlas.Initialize("Arial", 14, 1.0f));
    const auto* degree = atlas.GetGlyph(CustomOverlay::kDegreeChar);
    ASSERT_GT(degree->width, 0);
    const auto* question = atlas.GetGlyph('?');
    EXPECT_NE(degree->x, question->x) << "would have been drawn as the unknown-character fallback";
    EXPECT_FALSE(atlas.GetGlyphSpans(CustomOverlay::kDegreeChar).empty());
    EXPECT_GT(atlas.GetGlyph(CustomOverlay::kDegreeChar, TextSize::Small)->width, 0);
    float width = 0.0f;
    atlas.CalcTextSize("\xB0", &width, nullptr);
    EXPECT_EQ(width, static_cast<float>(degree->xAdvance));
}

// Everything must fit whatever the scale: the atlas sizes itself rather than dropping glyphs.
TEST(CustomFontAtlas, NoGlyphOverlapsAnotherAtAnyScale) {
    for (const float scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
        FontAtlas atlas;
        ASSERT_TRUE(atlas.Initialize("Arial", 14, scale));
        struct Cell {
            int x, y, w, h;
        };
        std::vector<Cell> cells;
        for (const TextSize size : {TextSize::Base, TextSize::Small, TextSize::Hero}) {
            for (int code = 32; code < 128; ++code) {
                const auto* glyph = atlas.GetGlyph(code == 127 ? CustomOverlay::kDegreeChar : static_cast<char>(code), size);
                if (size != TextSize::Hero || (code >= '0' && code <= '9'))
                    EXPECT_GT(glyph->width, 0) << code << " scale " << scale;
                if (glyph->width)
                    cells.push_back({glyph->x, glyph->y, glyph->width, glyph->height});
            }
        }
        // The '?' fallback in the hero table returns the same empty glyph, so duplicates are absent here.
        for (size_t a = 0; a < cells.size(); ++a) {
            EXPECT_LE(cells[a].x + cells[a].w, atlas.GetTextureWidth());
            EXPECT_LE(cells[a].y + cells[a].h, atlas.GetTextureHeight() - 4);
            for (size_t b = a + 1; b < cells.size(); ++b) {
                const bool apart = cells[a].x + cells[a].w <= cells[b].x || cells[b].x + cells[b].w <= cells[a].x ||
                                   cells[a].y + cells[a].h <= cells[b].y || cells[b].y + cells[b].h <= cells[a].y;
                EXPECT_TRUE(apart) << a << " and " << b << " scale " << scale;
            }
        }
    }
}

// CreateFont substitutes silently, which is why the old "fall back to Arial if Segoe UI is missing" could
// never run: the selected face is the only proof a font exists.
TEST(CustomFontAtlas, AMissingFontFailsUnlessASubstituteIsAccepted) {
    FontAtlas missing;
    EXPECT_FALSE(missing.Initialize("CE No Such Font Face", 14, 1.0f));
    EXPECT_FALSE(missing.IsInitialized());
    EXPECT_TRUE(missing.Initialize("CE No Such Font Face", 14, 1.0f, 0, true));
    EXPECT_FALSE(missing.GetFaceName().empty());
    EXPECT_NE(missing.GetFaceName(), "CE No Such Font Face");
    FontAtlas strict;
    EXPECT_FALSE(strict.Initialize("CE No Such Font Face", 14, 1.0f));

    FontAtlas truncated;
    if (truncated.Initialize("Bahnschrift SemiBold SemiConden", 14, 1.0f)) {
        EXPECT_EQ(truncated.GetFaceName().rfind("Bahnschrift SemiBold SemiConden", 0), 0u)
            << "GDI's 31 character name selects the full face";
        FontAtlas fullName;
        EXPECT_TRUE(fullName.Initialize("Bahnschrift SemiBold SemiCondensed", 14, 1.0f))
            << "config.ini.template suggests the name as Windows lists it, which is longer than GDI reads";
    }
}

TEST(CustomFontAtlasCache, WeightIsPartOfTheKey) {
    FontAtlas normal;
    ASSERT_TRUE(normal.Initialize("Arial", 17, 1.0f, 400));
    const uint32_t before = FontAtlas::RasterizedAtlasCount();
    FontAtlas bold;
    ASSERT_TRUE(bold.Initialize("Arial", 17, 1.0f, 700));
    EXPECT_EQ(FontAtlas::RasterizedAtlasCount(), before + 1);
    EXPECT_GT(bold.GetGlyph('W')->width + bold.GetGlyph('W')->xAdvance,
              normal.GetGlyph('W')->width + normal.GetGlyph('W')->xAdvance - 1);
    EXPECT_EQ(normal.GetWeight(), 400);
    EXPECT_EQ(bold.GetWeight(), 700);
}

TEST(OverlayFontPolicy, FacesNamingAWeightAreDrawnAsDesigned) {
    using ce::overlay_font::ResolveWeight;
    EXPECT_EQ(ResolveWeight("Bahnschrift SemiBold", 0), 400);
    EXPECT_EQ(ResolveWeight("Segoe UI Semibold", 0), 400);
    EXPECT_EQ(ResolveWeight("Cascadia Mono Light", 0), 400);
    EXPECT_EQ(ResolveWeight("Arial Black", 0), 400);
    EXPECT_EQ(ResolveWeight("Consolas", 0), 700);
    EXPECT_EQ(ResolveWeight("Segoe UI", 0), 700);
    EXPECT_EQ(ResolveWeight("Consolas", 300), 300) << "an explicit weight wins";
    EXPECT_EQ(ResolveWeight("Bahnschrift SemiBold", 700), 700);
}

TEST(OverlayFontPolicy, FaceMatchReadsWhatGdiReads) {
    using ce::overlay_font::FaceMatches;
    EXPECT_TRUE(FaceMatches(L"segoe ui", L"Segoe UI"));
    EXPECT_TRUE(FaceMatches(L"Bahnschrift SemiBold SemiConden", L"Bahnschrift SemiBold SemiCondensed"));
    EXPECT_TRUE(FaceMatches(L"Bahnschrift SemiBold SemiCondensed", L"Bahnschrift SemiBold SemiCondensed"));
    EXPECT_FALSE(FaceMatches(L"No Such Font", L"Arial"));
    EXPECT_FALSE(FaceMatches(L"", L"Arial"));
    EXPECT_FALSE(FaceMatches(L"Segoe", L"Segoe UI"));
    EXPECT_FALSE(FaceMatches(L"Segoe UI Semibold", L"Segoe UI"));
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
