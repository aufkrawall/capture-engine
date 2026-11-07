/**
 * Custom Overlay Font Atlas
 *
 * Lightweight bitmap font atlas for overlay text rendering.
 * Replaces ImGui's font system with a minimal implementation.
 *
 * Uses embedded ASCII bitmap font data (no external files needed).
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace CustomOverlay {

// Glyph information for a single character
struct Glyph {
    uint16_t x, y;             // Position in atlas texture
    uint16_t width, height;    // Size of glyph
    int16_t xOffset, yOffset;  // Offset from cursor position
    uint16_t xAdvance;         // Horizontal advance after drawing
};

// A rectangle of one coverage value. Equal runs in consecutive rows share one span, so a stem is one
// quad and not one per pixel row.
struct GlyphSpan {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t alpha;
};

// The sizes one atlas holds, so text of any size shares one texture and one draw call. Each is rasterized
// at its own pixel size rather than scaled, which keeps it sharp. Base and Small hold every printable
// character plus the degree sign; Hero holds digits only, because the layouts draw nothing else that large.
enum class TextSize : uint8_t { Base = 0, Small = 1, Hero = 2 };
inline constexpr int kTextSizeCount = 3;

// The degree sign in overlay text. Its glyph sits in the unused DEL slot of the 128 entry tables.
inline constexpr char kDegreeChar = static_cast<char>(0xB0);

struct TextSizeMetrics {
    int pixelSize = 0;
    int lineHeight = 0;
    int ascent = 0;
    int digitHeight = 0;  // Ink height of a digit, what rows center on: line height varies a lot by font.
};

// Font atlas containing bitmap font data
class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();

    // Rasterizes a system font with Windows GDI. False when the font is not installed, because GDI would
    // silently substitute another one; acceptSubstitute takes whatever GDI selects instead. A weight of 0
    // applies ce::overlay_font::ResolveWeight.
    bool Initialize(const char* fontName = "Segoe UI", int fontSize = 14, float dpiScale = 1.0f, int weight = 0,
                    bool acceptSubstitute = false);
    void Shutdown();
    bool IsInitialized() const {
        return initialized;
    }

    // Get glyph info for a character
    const Glyph* GetGlyph(char c, TextSize size = TextSize::Base) const;
    const std::vector<GlyphSpan>& GetGlyphSpans(char c, TextSize size = TextSize::Base) const;

    // Get atlas texture data (RGBA)
    const uint8_t* GetTextureData() const {
        return textureData.data();
    }
    int GetTextureWidth() const {
        return textureWidth;
    }
    int GetTextureHeight() const {
        return textureHeight;
    }
    // Inside a block of opaque white that no glyph comes near: sampling here returns exactly 1 under
    // any filter, so solid geometry can draw through the font texture and share the text's draw call.
    float GetWhiteU() const {
        return (static_cast<float>(kWhiteBlockSize) * 0.5f) / static_cast<float>(textureWidth);
    }
    float GetWhiteV() const {
        return (static_cast<float>(textureHeight) - static_cast<float>(kWhiteBlockSize) * 0.5f) /
               static_cast<float>(textureHeight);
    }

    // Font metrics
    int GetLineHeight(TextSize size = TextSize::Base) const {
        return sets[static_cast<int>(size)].metrics.lineHeight;
    }
    const TextSizeMetrics& GetMetrics(TextSize size) const {
        return sets[static_cast<int>(size)].metrics;
    }
    float GetDpiScale() const {
        return dpiScale;
    }
    // The face GDI selected, as UTF-8.
    const std::string& GetFaceName() const {
        return faceName;
    }
    int GetWeight() const {
        return weight;
    }

    // Calculate text dimensions
    void CalcTextSize(const char* text, float* outWidth, float* outHeight, TextSize size = TextSize::Base) const;

    // Process-wide count of atlases rasterized with GDI; cache hits do not add to it.
    static uint32_t RasterizedAtlasCount();

    // One size's glyph tables, indexed by slot: the character code, the degree sign in the DEL slot.
    struct GlyphSet {
        TextSizeMetrics metrics;
        Glyph glyphs[128] = {};
        std::vector<GlyphSpan> spans[128];
    };

private:
    // In the bottom-left corner, the part of the atlas glyph packing reaches last.
    static constexpr int kWhiteBlockSize = 4;

    bool Rasterize(const wchar_t* face, int fontSize, float scale, int resolvedWeight, bool acceptSubstitute);
    void BuildGlyphSpans(GlyphSet& set);
    // Shares the finished atlas with later renderers (see the cache in custom_font.cpp).
    void PublishSnapshot(const char* fontName, int fontSize, float scale, bool acceptSubstitute) const;

    std::vector<uint8_t> textureData;  // RGBA pixel data
    int textureWidth = 0;
    int textureHeight = 0;
    float dpiScale = 1.0f;
    int weight = 0;
    std::string faceName;

    GlyphSet sets[kTextSizeCount];
    bool initialized = false;
};

// Color utilities (replaces ImGui color functions)
inline uint32_t ColorRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;  // ABGR format (matches ImGui)
}

inline uint32_t ColorScale(uint32_t col, float scale) {
    if (scale <= 1.0f)
        return col;
    uint8_t r = (col >> 0) & 0xFF;
    uint8_t g = (col >> 8) & 0xFF;
    uint8_t b = (col >> 16) & 0xFF;
    uint8_t a = (col >> 24) & 0xFF;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    r = (uint8_t)((r * scale > 255) ? 255 : (int)(r * scale));
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g = (uint8_t)((g * scale > 255) ? 255 : (int)(g * scale));
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    b = (uint8_t)((b * scale > 255) ? 255 : (int)(b * scale));
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Extract color components
inline void ColorToRGBA(uint32_t col, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (col >> 0) & 0xFF;
    g = (col >> 8) & 0xFF;
    b = (col >> 16) & 0xFF;
    a = (col >> 24) & 0xFF;
}

}  // namespace CustomOverlay
