/**
 * Custom Overlay Font Atlas
 *
 * Lightweight bitmap font atlas for overlay text rendering.
 * Replaces ImGui's font system with a minimal implementation.
 *
 * Uses embedded ASCII bitmap font data (no external files needed).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace CustomOverlay {

// Glyph information for a single character
struct Glyph {
  uint16_t x, y;            // Position in atlas texture
  uint16_t width, height;   // Size of glyph
  int16_t xOffset, yOffset; // Offset from cursor position
  uint16_t xAdvance;        // Horizontal advance after drawing
};

// Font atlas containing bitmap font data
class FontAtlas {
public:
  FontAtlas();
  ~FontAtlas();

  // Initialize font atlas with a specific size
  // Uses Windows GDI to rasterize a system font
  bool Initialize(const char *fontName = "Segoe UI", int fontSize = 14,
                  float dpiScale = 1.0f);
  void Shutdown();

  // Get glyph info for a character (ASCII only for now)
  const Glyph *GetGlyph(char c) const;

  // Get atlas texture data (RGBA)
  const uint8_t *GetTextureData() const { return textureData.data(); }
  int GetTextureWidth() const { return textureWidth; }
  int GetTextureHeight() const { return textureHeight; }

  // Font metrics
  int GetLineHeight() const { return lineHeight; }
  float GetDpiScale() const { return dpiScale; }

  // Calculate text dimensions
  void CalcTextSize(const char *text, float *outWidth, float *outHeight) const;

private:
  std::vector<uint8_t> textureData; // RGBA pixel data
  int textureWidth = 0;
  int textureHeight = 0;
  int lineHeight = 0;
  float dpiScale = 1.0f;

  Glyph glyphs[128]; // ASCII glyphs
  bool initialized = false;
};

// Color utilities (replaces ImGui color functions)
inline uint32_t ColorRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (a << 24) | (b << 16) | (g << 8) | r; // ABGR format (matches ImGui)
}

inline uint32_t ColorScale(uint32_t col, float scale) {
  if (scale <= 1.0f)
    return col;
  uint8_t r = (col >> 0) & 0xFF;
  uint8_t g = (col >> 8) & 0xFF;
  uint8_t b = (col >> 16) & 0xFF;
  uint8_t a = (col >> 24) & 0xFF;
  r = (uint8_t)((r * scale > 255) ? 255 : (int)(r * scale));
  g = (uint8_t)((g * scale > 255) ? 255 : (int)(g * scale));
  b = (uint8_t)((b * scale > 255) ? 255 : (int)(b * scale));
  return (a << 24) | (b << 16) | (g << 8) | r;
}

// Extract color components
inline void ColorToRGBA(uint32_t col, uint8_t &r, uint8_t &g, uint8_t &b,
                        uint8_t &a) {
  r = (col >> 0) & 0xFF;
  g = (col >> 8) & 0xFF;
  b = (col >> 16) & 0xFF;
  a = (col >> 24) & 0xFF;
}

} // namespace CustomOverlay
