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

// Font atlas containing bitmap font data
class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();

    // Initialize font atlas with a specific size
    // Uses Windows GDI to rasterize a system font
    bool Initialize(const char* fontName = "Segoe UI", int fontSize = 14, float dpiScale = 1.0f);
    void Shutdown();
    bool IsInitialized() const {
        return initialized;
    }

    // Get glyph info for a character (ASCII only for now)
    const Glyph* GetGlyph(char c) const;
    const std::vector<GlyphSpan>& GetGlyphSpans(char c) const;

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
    int GetLineHeight() const {
        return lineHeight;
    }
    float GetDpiScale() const {
        return dpiScale;
    }

    // Calculate text dimensions
    void CalcTextSize(const char* text, float* outWidth, float* outHeight) const;

    // Process-wide count of atlases rasterized with GDI; cache hits do not add to it.
    static uint32_t RasterizedAtlasCount();

private:
    // In the bottom-left corner, the part of the atlas glyph packing reaches last.
    static constexpr int kWhiteBlockSize = 4;

    void BuildGlyphSpans();
    // Shares the finished atlas with later renderers (see the cache in custom_font.cpp).
    void PublishSnapshot(const char* fontName, int fontSize, float scale) const;

    std::vector<uint8_t> textureData;  // RGBA pixel data
    int textureWidth = 0;
    int textureHeight = 0;
    int lineHeight = 0;
    float dpiScale = 1.0f;

    Glyph glyphs[128];  // ASCII glyphs
    std::vector<GlyphSpan> glyphSpans[128];
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
