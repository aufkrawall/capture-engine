/**
 * Custom Overlay Renderer Implementation
 */

#include "custom_overlay.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace CustomOverlay {

Renderer::Renderer() {}

Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(RendererBackend *backendPtr, float scale) {
  if (initialized)
    return true;
  if (!backendPtr)
    return false;

  backend = backendPtr;
  dpiScale = scale;

  // Initialize font atlas
  if (!fontAtlas.Initialize("Segoe UI", 14, scale)) {
    // Fallback to Arial if Segoe UI not available
    if (!fontAtlas.Initialize("Arial", 14, scale)) {
      return false;
    }
  }

  // Initialize backend with font texture
  if (!backend->Initialize(fontAtlas.GetTextureWidth(),
                           fontAtlas.GetTextureHeight(),
                           fontAtlas.GetTextureData())) {
    fontAtlas.Shutdown();
    return false;
  }

  initialized = true;
  return true;
}

void Renderer::Shutdown() {
  if (backend) {
    backend->Shutdown();
  }
  fontAtlas.Shutdown();
  vertices.clear();
  indices.clear();
  commands.clear();
  initialized = false;
}

void Renderer::BeginFrame(int width, int height) {
  if (!initialized)
    return;

  viewportWidth = width;
  viewportHeight = height;

  vertices.clear();
  indices.clear();
  commands.clear();

  cursorX = 0;
  cursorY = 0;
  inWindow = false;
  inTable = false;
  frameStarted = true;
}

void Renderer::EndFrame() {
  if (!initialized || !frameStarted)
    return;

  // Render accumulated geometry
  if (!commands.empty() && backend) {
    backend->Render(vertices, indices, commands, viewportWidth, viewportHeight);
  }

  frameStarted = false;
}

void Renderer::AddQuad(float x, float y, float w, float h, float u0, float v0,
                       float u1, float v1, uint32_t color) {
  uint16_t baseIdx = (uint16_t)vertices.size();

  // Add vertices (4 corners)
  vertices.push_back({x, y, u0, v0, color});
  vertices.push_back({x + w, y, u1, v0, color});
  vertices.push_back({x + w, y + h, u1, v1, color});
  vertices.push_back({x, y + h, u0, v1, color});

  // Add indices (2 triangles)
  indices.push_back(baseIdx + 0);
  indices.push_back(baseIdx + 1);
  indices.push_back(baseIdx + 2);
  indices.push_back(baseIdx + 0);
  indices.push_back(baseIdx + 2);
  indices.push_back(baseIdx + 3);
}

void Renderer::AddTextQuads(float x, float y, const char *text,
                            uint32_t color) {
  if (!text)
    return;

  float atlasW = (float)fontAtlas.GetTextureWidth();
  float atlasH = (float)fontAtlas.GetTextureHeight();

  float px = x;
  float py = y;

  while (*text) {
    char c = *text++;

    if (c == '\n') {
      px = x;
      py += fontAtlas.GetLineHeight();
      continue;
    }

    const Glyph *g = fontAtlas.GetGlyph(c);
    if (!g || g->width == 0)
      continue;

    float gx = px + g->xOffset;
    float gy = py + g->yOffset;
    float gw = (float)g->width;
    float gh = (float)g->height;

    // Texture coordinates - atlas uses top-left origin (Windows GDI style)
    float u0 = g->x / atlasW;
    float v0 = g->y / atlasH;
    float u1 = (g->x + g->width) / atlasW;
    float v1 = (g->y + g->height) / atlasH;

    AddQuad(gx, gy, gw, gh, u0, v0, u1, v1, color);

    px += g->xAdvance;
  }
}

void Renderer::FlushBatch(bool useTexture) {
  if (vertices.empty())
    return;

  // Check if we can merge with previous command
  if (!commands.empty() && commands.back().useTexture == useTexture) {
    // Extend previous command
    commands.back().vertexCount =
        (uint32_t)vertices.size() - commands.back().vertexOffset;
    commands.back().indexCount =
        (uint32_t)indices.size() - commands.back().indexOffset;
  } else {
    // New command
    DrawCommand cmd = {};
    cmd.vertexOffset = 0;
    cmd.vertexCount = (uint32_t)vertices.size();
    cmd.indexOffset = 0;
    cmd.indexCount = (uint32_t)indices.size();
    cmd.useTexture = useTexture;

    if (!commands.empty()) {
      auto &prev = commands.back();
      cmd.vertexOffset = prev.vertexOffset + prev.vertexCount;
      cmd.indexOffset = prev.indexOffset + prev.indexCount;
      cmd.vertexCount = (uint32_t)vertices.size() - cmd.vertexOffset;
      cmd.indexCount = (uint32_t)indices.size() - cmd.indexOffset;
    }

    commands.push_back(cmd);
  }
}

void Renderer::DrawText(float x, float y, const char *text, uint32_t color) {
  if (!initialized || !text)
    return;

  size_t prevVertCount = vertices.size();
  AddTextQuads(x, y, text, color);

  if (vertices.size() > prevVertCount) {
    FlushBatch(true); // Text uses texture
  }
}

void Renderer::DrawTextWithShadow(float x, float y, const char *text,
                                  uint32_t color, uint32_t shadowColor,
                                  float shadowOffset) {
  if (!initialized || !text)
    return;

  // Draw shadow first
  AddTextQuads(x + shadowOffset, y + shadowOffset, text, shadowColor);
  // Draw main text on top
  AddTextQuads(x, y, text, color);
  FlushBatch(true);
}

void Renderer::CalcTextSize(const char *text, float *outWidth,
                            float *outHeight) const {
  fontAtlas.CalcTextSize(text, outWidth, outHeight);
}

void Renderer::DrawRect(float x, float y, float w, float h, uint32_t color) {
  // Draw outline as 4 thin rectangles
  float thickness = dpiScale;
  DrawRectFilled(x, y, w, thickness, color);                 // Top
  DrawRectFilled(x, y + h - thickness, w, thickness, color); // Bottom
  DrawRectFilled(x, y, thickness, h, color);                 // Left
  DrawRectFilled(x + w - thickness, y, thickness, h, color); // Right
}

void Renderer::DrawRectFilled(float x, float y, float w, float h,
                              uint32_t color) {
  if (!initialized)
    return;

  // Use texture coordinates outside atlas (solid white area)
  // Or just use (0,0) which should be white in our font texture
  AddQuad(x, y, w, h, 0, 0, 0, 0, color);
  FlushBatch(false); // Solid color, no texture sampling needed for color
}

void Renderer::BeginWindow(float x, float y, uint32_t bgColor, float alpha) {
  if (!initialized)
    return;

  windowX = x;
  windowY = y;
  cursorX = x + 8 * dpiScale; // Padding
  cursorY = y + 4 * dpiScale;
  inWindow = true;

  // Background will be drawn in EndWindow after we know the size
}

void Renderer::EndWindow() {
  if (!initialized || !inWindow)
    return;
  inWindow = false;
}

void Renderer::BeginTable() {
  if (!initialized)
    return;
  inTable = true;
  tableStartY = cursorY;
}

void Renderer::TableRow(const char *label, const char *value,
                        uint32_t labelColor, uint32_t valueColor) {
  if (!initialized || !inTable)
    return;

  float colWidth = 100 * dpiScale; // Fixed column width for label

  // Draw label
  DrawText(cursorX, cursorY, label, labelColor);

  // Draw value
  DrawText(cursorX + colWidth, cursorY, value, valueColor);

  // Move to next row
  cursorY += fontAtlas.GetLineHeight();
}

void Renderer::EndTable() {
  if (!initialized)
    return;
  inTable = false;
}

} // namespace CustomOverlay
