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

  // Pre-reserve buffers to avoid per-frame heap allocations.
  // Typical overlay: ~200 text glyphs (4 verts each) + graph (360 verts) +
  // rects = ~1200 verts
  vertices.reserve(4096);
  indices.reserve(8192);
  commands.reserve(32);

  initialized = true;
  return true;
}

void Renderer::Shutdown() {
  if (!initialized)
    return;
  if (backend && !skipDeviceRelease) {
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

  currentBatchVertexOffset = 0;
  currentBatchIndexOffset = 0;
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
  AddTextQuadsScaled(x, y, text, color, 1.0f);
}

void Renderer::AddTextQuadsScaled(float x, float y, const char *text,
                                  uint32_t color, float scale) {
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
      py += fontAtlas.GetLineHeight() * scale;
      continue;
    }

    const Glyph *g = fontAtlas.GetGlyph(c);
    if (!g || g->width == 0)
      continue;

    float gx = px + g->xOffset * scale;
    float gy = py + g->yOffset * scale;
    float gw = (float)g->width * scale;
    float gh = (float)g->height * scale;

    float u0 = g->x / atlasW;
    float u1 = (g->x + g->width) / atlasW;
    float v0 = g->y / atlasH;
    float v1 = (g->y + g->height) / atlasH;

    AddQuad(gx, gy, gw, gh, u0, v0, u1, v1, color);

    px += g->xAdvance * scale;
  }
}

void Renderer::FlushBatch(bool useTexture) {
  if (vertices.empty())
    return;

  // Save current batch starting offsets before clearing
  uint32_t batchVertexStart = currentBatchVertexOffset;
  uint32_t batchIndexStart = currentBatchIndexOffset;
  uint32_t batchVertexCount = (uint32_t)vertices.size() - batchVertexStart;
  uint32_t batchIndexCount = (uint32_t)indices.size() - batchIndexStart;

  // Check if we can merge with previous command
  if (!commands.empty() && commands.back().useTexture == useTexture) {
    // Extend previous command - keep its original offsets but add our counts
    commands.back().vertexCount += batchVertexCount;
    commands.back().indexCount += batchIndexCount;
  } else {
    // New command
    DrawCommand cmd = {};
    cmd.vertexOffset = batchVertexStart;
    cmd.vertexCount = batchVertexCount;
    cmd.indexOffset = batchIndexStart;
    cmd.indexCount = batchIndexCount;
    cmd.useTexture = useTexture;
    commands.push_back(cmd);
  }

  // Update batch offsets for next AddX call
  currentBatchVertexOffset = (uint32_t)vertices.size();
  currentBatchIndexOffset = (uint32_t)indices.size();
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

void Renderer::DrawTextScaled(float x, float y, const char *text,
                              uint32_t color, float scale) {
  if (!initialized || !text)
    return;

  size_t prevVertCount = vertices.size();
  AddTextQuadsScaled(x, y, text, color, scale);

  if (vertices.size() > prevVertCount) {
    FlushBatch(true); // Text uses texture
  }
}

void Renderer::DrawTextScaledWithShadow(float x, float y, const char *text,
                                        uint32_t color, uint32_t shadowColor,
                                        float scale, float shadowOffset) {
  if (!initialized || !text)
    return;

  // Draw shadow first
  AddTextQuadsScaled(x + shadowOffset, y + shadowOffset, text, shadowColor,
                     scale);
  // Draw main text on top
  AddTextQuadsScaled(x, y, text, color, scale);
  FlushBatch(true);
}

void Renderer::DrawTextRightAligned(float rightX, float y, const char *text,
                                    uint32_t color, uint32_t shadowColor,
                                    float shadowOffset) {
  if (!initialized || !text)
    return;
  float tw = 0, th = 0;
  CalcTextSize(text, &tw, &th);
  float x = std::floor(rightX - tw); // Snap to pixel to prevent sub-pixel artifacts
  DrawTextWithShadow(x, y, text, color, shadowColor, shadowOffset);
}

void Renderer::DrawTextScaledRightAligned(float rightX, float y,
                                          const char *text, uint32_t color,
                                          uint32_t shadowColor, float scale,
                                          float shadowOffset) {
  if (!initialized || !text)
    return;
  float tw = 0, th = 0;
  CalcTextSizeScaled(text, &tw, &th, scale);
  float x = std::floor(rightX - tw); // Snap to pixel to prevent sub-pixel artifacts
  DrawTextScaledWithShadow(x, y, text, color, shadowColor, scale,
                           shadowOffset);
}

void Renderer::CalcTextSize(const char *text, float *outWidth,
                            float *outHeight) const {
  fontAtlas.CalcTextSize(text, outWidth, outHeight);
}

void Renderer::CalcTextSizeScaled(const char *text, float *outWidth,
                                  float *outHeight, float scale) const {
  fontAtlas.CalcTextSize(text, outWidth, outHeight);
  if (outWidth)
    *outWidth *= scale;
  if (outHeight)
    *outHeight *= scale;
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

void Renderer::DrawLine(float x0, float y0, float x1, float y1, uint32_t color,
                        float thickness) {
  if (!initialized)
    return;

  // Calculate perpendicular direction for line thickness
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len < 0.001f)
    return;

  // Normalize and get perpendicular
  float nx = -dy / len * (thickness * 0.5f);
  float ny = dx / len * (thickness * 0.5f);

  // Create quad vertices for the line
  uint16_t baseIdx = (uint16_t)vertices.size();
  vertices.push_back({x0 + nx, y0 + ny, 0, 0, color});
  vertices.push_back({x0 - nx, y0 - ny, 0, 0, color});
  vertices.push_back({x1 - nx, y1 - ny, 0, 0, color});
  vertices.push_back({x1 + nx, y1 + ny, 0, 0, color});

  // Two triangles for the quad
  indices.push_back(baseIdx + 0);
  indices.push_back(baseIdx + 1);
  indices.push_back(baseIdx + 2);
  indices.push_back(baseIdx + 0);
  indices.push_back(baseIdx + 2);
  indices.push_back(baseIdx + 3);

  FlushBatch(false);
}

void Renderer::DrawGraphPolyline(const float *xs, const float *ys, int count,
                                  uint32_t color, float thickness) {
  if (count < 2)
    return;

  const float AA_SIZE = 0.5f * dpiScale;
  const float halfThick = thickness * 0.5f;
  const uint32_t colorAA = color & 0x00FFFFFFu;

  auto getNormal = [&](int i, float &nx, float &ny) {
    if (i == 0) {
      float dx = xs[1] - xs[0], dy = ys[1] - ys[0];
      float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.001f) { nx = 0.0f; ny = 1.0f; }
      else { nx = -dy / len; ny = dx / len; }
    } else if (i == count - 1) {
      float dx = xs[i] - xs[i - 1], dy = ys[i] - ys[i - 1];
      float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.001f) { nx = 0.0f; ny = 1.0f; }
      else { nx = -dy / len; ny = dx / len; }
    } else {
      float dx0 = xs[i] - xs[i - 1], dy0 = ys[i] - ys[i - 1];
      float dx1 = xs[i + 1] - xs[i], dy1 = ys[i + 1] - ys[i];
      float len0 = std::sqrt(dx0 * dx0 + dy0 * dy0);
      float len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
      if (len0 < 0.001f && len1 < 0.001f) { nx = 0.0f; ny = 1.0f; }
      else if (len0 < 0.001f) { nx = -dy1 / len1; ny = dx1 / len1; }
      else if (len1 < 0.001f) { nx = -dy0 / len0; ny = dx0 / len0; }
      else {
        float n0x = -dy0 / len0, n0y = dx0 / len0;
        float n1x = -dy1 / len1, n1y = dx1 / len1;
        float mx = n0x + n1x, my = n0y + n1y;
        float mLen = std::sqrt(mx * mx + my * my);
        if (mLen < 0.001f) { nx = n0x; ny = n0y; }
        else {
          float dot = n0x * mx / mLen + n0y * my / mLen;
          float miterScale = (std::abs(dot) > 0.01f) ? 1.0f / dot : 1.0f;
          miterScale = (std::min)(miterScale, 4.0f);
          nx = mx / mLen * miterScale;
          ny = my / mLen * miterScale;
        }
      }
    }
  };

  for (int i = 0; i < count - 1; i++) {
    float n0x, n0y, n1x, n1y;
    getNormal(i, n0x, n0y);
    getNormal(i + 1, n1x, n1y);

    const float ox0 = n0x * (halfThick + AA_SIZE), oy0 = n0y * (halfThick + AA_SIZE);
    const float ix0 = n0x * halfThick, iy0 = n0y * halfThick;
    const float ox1 = n1x * (halfThick + AA_SIZE), oy1 = n1y * (halfThick + AA_SIZE);
    const float ix1 = n1x * halfThick, iy1 = n1y * halfThick;

    const uint16_t baseIdx = (uint16_t)vertices.size();
    vertices.push_back({xs[i] + ox0, ys[i] + oy0, 0, 0, colorAA});
    vertices.push_back({xs[i] + ix0, ys[i] + iy0, 0, 0, color});
    vertices.push_back({xs[i] - ix0, ys[i] - iy0, 0, 0, color});
    vertices.push_back({xs[i] - ox0, ys[i] - oy0, 0, 0, colorAA});
    vertices.push_back({xs[i + 1] + ox1, ys[i + 1] + oy1, 0, 0, colorAA});
    vertices.push_back({xs[i + 1] + ix1, ys[i + 1] + iy1, 0, 0, color});
    vertices.push_back({xs[i + 1] - ix1, ys[i + 1] - iy1, 0, 0, color});
    vertices.push_back({xs[i + 1] - ox1, ys[i + 1] - oy1, 0, 0, colorAA});

    indices.push_back(baseIdx + 0); indices.push_back(baseIdx + 1); indices.push_back(baseIdx + 5);
    indices.push_back(baseIdx + 0); indices.push_back(baseIdx + 5); indices.push_back(baseIdx + 4);
    indices.push_back(baseIdx + 1); indices.push_back(baseIdx + 2); indices.push_back(baseIdx + 6);
    indices.push_back(baseIdx + 1); indices.push_back(baseIdx + 6); indices.push_back(baseIdx + 5);
    indices.push_back(baseIdx + 2); indices.push_back(baseIdx + 3); indices.push_back(baseIdx + 7);
    indices.push_back(baseIdx + 2); indices.push_back(baseIdx + 7); indices.push_back(baseIdx + 6);
  }

  FlushBatch(false);
}

void Renderer::DrawFrameTimeGraph(float x, float y, float width, float height,
                                  const float *frameTimes, int count,
                                  float minVal, float maxVal, uint32_t color) {
  if (!initialized || !frameTimes || count < 2)
    return;

  float range = maxVal - minVal;
  if (range < 0.001f)
    range = 33.33f;

  // Pre-compute screen positions into stack-allocated arrays.
  constexpr int kMaxPoints = 1024;
  if (count > kMaxPoints)
    count = kMaxPoints;

  float xs[kMaxPoints], ys[kMaxPoints];
  const float stepX = width / (float)(count - 1);
  const float invRange = 1.0f / range;

  for (int i = 0; i < count; i++) {
    float val = frameTimes[i];
    val = (std::max)(minVal, (std::min)(maxVal, val));
    xs[i] = x + (float)i * stepX;
    ys[i] = y + height - ((val - minVal) * invRange) * height;
  }

  // Skip leading zero-value samples (unfilled ring buffer at startup).
  int firstValid = 0;
  while (firstValid < count - 1 && frameTimes[firstValid] <= 0.0f)
    firstValid++;
  const int validCount = count - firstValid;
  if (validCount < 2)
    return;

  const float *vxs = xs + firstValid;
  const float *vys = ys + firstValid;

  // Connected polyline with round joins + 1px AA fringe.
  // 0.75 logical pixels at every DPI (e.g. 100%→0.75px, 200%→1.5px physical = same visual size).
  const float lineThickness = 0.75f * dpiScale;
  DrawGraphPolyline(vxs, vys, validCount, color, lineThickness);
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
