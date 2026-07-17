/**
 * Custom Overlay Renderer Implementation
 */

#include "custom_overlay.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "hook_common.h"

namespace CustomOverlay {

namespace {

uint32_t ApplyCoverageAlpha(uint32_t color, uint8_t coverage) {
    const uint32_t alpha = (color >> 24) & 0xFFu;
    const uint32_t coveredAlpha = (alpha * (uint32_t)coverage + 127u) / 255u;
    return (color & 0x00FFFFFFu) | (coveredAlpha << 24);
}

}  // namespace

Renderer::Renderer() {}

Renderer::~Renderer() {
#ifndef VK_LAYER_CE_OVERLAY
    if (IsProcessTerminating())
        return;
#endif
    Shutdown();
}

bool Renderer::Initialize(RendererBackend* backendPtr, float scale) {
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
    if (!backend->Initialize(fontAtlas.GetTextureWidth(), fontAtlas.GetTextureHeight(), fontAtlas.GetTextureData())) {
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

    if (backend) {
        backend->OnDrawDataChanged();
    }

    // Render accumulated geometry
    if (!commands.empty() && backend) {
        backend->Render(vertices, indices, commands, viewportWidth, viewportHeight);
    }

    frameStarted = false;
}

bool Renderer::RenderCachedFrame(int width, int height) {
    if (!initialized || !backend || commands.empty())
        return false;

    viewportWidth = width;
    viewportHeight = height;
    backend->Render(vertices, indices, commands, viewportWidth, viewportHeight);
    return true;
}

void Renderer::AddQuad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color) {
    if (vertices.size() > 0xFFFFu - 4u) {
        static std::atomic<int> s_overflowLogCount{0};
        const int logN = s_overflowLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logN < 4 || (logN % 200) == 0) {
            HookLogImportant("Overlay renderer: skipped quad because 16-bit vertex indices would overflow (verts=%zu)",
                             vertices.size());
        }
        return;
    }

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

void Renderer::AddTextQuads(float x, float y, const char* text, uint32_t color) {
    AddTextQuadsScaled(x, y, text, color, 1.0f);
}

void Renderer::AddTextQuadsScaled(float x, float y, const char* text, uint32_t color, float scale) {
    if (!text)
        return;

    float atlasW = (float)fontAtlas.GetTextureWidth();
    float atlasH = (float)fontAtlas.GetTextureHeight();

    // Snap origin to nearest integer pixel so each glyph samples its atlas texel
    // center exactly under linear filtering — prevents sub-pixel blur.
    const float originX = roundf(x);
    float px = originX;
    float py = roundf(y);

    while (*text) {
        char c = *text++;

        if (c == '\n') {
            px = originX;
            py += static_cast<float>(fontAtlas.GetLineHeight()) * scale;
            continue;
        }

        const Glyph* g = fontAtlas.GetGlyph(c);
        if (!g || g->width == 0)
            continue;

        float gx = px + static_cast<float>(g->xOffset) * scale;
        float gy = py + static_cast<float>(g->yOffset) * scale;
        float gw = (float)g->width * scale;
        float gh = (float)g->height * scale;

        float u0 = static_cast<float>(g->x) / atlasW;
        float u1 = static_cast<float>(g->x + g->width) / atlasW;
        float v0 = static_cast<float>(g->y) / atlasH;
        float v1 = static_cast<float>(g->y + g->height) / atlasH;

        AddQuad(gx, gy, gw, gh, u0, v0, u1, v1, color);

        px += static_cast<float>(g->xAdvance) * scale;
    }
}

void Renderer::AddTextSolidQuads(float x, float y, const char* text, uint32_t color) {
    AddTextSolidQuadsScaled(x, y, text, color, 1.0f);
}

void Renderer::AddTextSolidQuadsScaled(float x, float y, const char* text, uint32_t color, float scale) {
    if (!text || scale <= 0.0f)
        return;

    const float originX = roundf(x);
    float px = originX;
    float py = roundf(y);

    while (*text) {
        char c = *text++;

        if (c == '\n') {
            px = originX;
            py += static_cast<float>(fontAtlas.GetLineHeight()) * scale;
            continue;
        }

        const Glyph* g = fontAtlas.GetGlyph(c);
        if (!g || g->width == 0)
            continue;

        const auto& spans = fontAtlas.GetGlyphSpans(c);
        if (!spans.empty()) {
            const float gx = px + static_cast<float>(g->xOffset) * scale;
            const float gy = py + static_cast<float>(g->yOffset) * scale;
            const float rowHeight = scale;

            for (const auto& span : spans) {
                const uint32_t spanColor = ApplyCoverageAlpha(color, span.alpha);
                if ((spanColor & 0xFF000000u) == 0)
                    continue;

                AddQuad(gx + (float)span.x * scale, gy + (float)span.y * scale, (float)span.width * scale, rowHeight,
                        0.0f, 0.0f, 0.0f, 0.0f, spanColor);
            }
        }

        px += static_cast<float>(g->xAdvance) * scale;
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

void Renderer::DrawText(float x, float y, const char* text, uint32_t color) {
    if (!initialized || !text)
        return;

    size_t prevVertCount = vertices.size();
    const bool solidText = backend && backend->PreferSolidTextGeometry();
    if (solidText) {
        AddTextSolidQuads(x, y, text, color);
    } else {
        AddTextQuads(x, y, text, color);
    }

    if (vertices.size() > prevVertCount) {
        FlushBatch(!solidText);
    }
}

void Renderer::DrawTextWithShadow(float x, float y, const char* text, uint32_t color, uint32_t shadowColor,
                                  float shadowOffset) {
    if (!initialized || !text)
        return;

    // Snap once, then derive both origins from that same physical-pixel anchor.
    // Independently rounded origins can alternate between a 1px and 2px shadow
    // at fractional DPI and resemble a stray underline.
    const float originX = roundf(x);
    const float originY = roundf(y);
    float off = roundf(shadowOffset * dpiScale);
    const bool solidText = backend && backend->PreferSolidTextGeometry();
    size_t prevVertCount = vertices.size();
    if (solidText) {
        AddTextSolidQuads(originX + off, originY + off, text, shadowColor);
        AddTextSolidQuads(originX, originY, text, color);
    } else {
        AddTextQuads(originX + off, originY + off, text, shadowColor);
        AddTextQuads(originX, originY, text, color);
    }
    if (vertices.size() > prevVertCount) {
        FlushBatch(!solidText);
    }
}

void Renderer::DrawTextScaled(float x, float y, const char* text, uint32_t color, float scale) {
    if (!initialized || !text)
        return;

    size_t prevVertCount = vertices.size();
    const bool solidText = backend && backend->PreferSolidTextGeometry();
    if (solidText) {
        AddTextSolidQuadsScaled(x, y, text, color, scale);
    } else {
        AddTextQuadsScaled(x, y, text, color, scale);
    }

    if (vertices.size() > prevVertCount) {
        FlushBatch(!solidText);
    }
}

void Renderer::DrawTextScaledWithShadow(float x, float y, const char* text, uint32_t color, uint32_t shadowColor,
                                        float scale, float shadowOffset) {
    if (!initialized || !text)
        return;

    const float originX = roundf(x);
    const float originY = roundf(y);
    float off = roundf(shadowOffset * dpiScale);
    const bool solidText = backend && backend->PreferSolidTextGeometry();
    size_t prevVertCount = vertices.size();
    if (solidText) {
        AddTextSolidQuadsScaled(originX + off, originY + off, text, shadowColor, scale);
        AddTextSolidQuadsScaled(originX, originY, text, color, scale);
    } else {
        AddTextQuadsScaled(originX + off, originY + off, text, shadowColor, scale);
        AddTextQuadsScaled(originX, originY, text, color, scale);
    }
    if (vertices.size() > prevVertCount) {
        FlushBatch(!solidText);
    }
}

void Renderer::DrawTextRightAligned(float rightX, float y, const char* text, uint32_t color, uint32_t shadowColor,
                                    float shadowOffset) {
    if (!initialized || !text)
        return;
    float tw = 0, th = 0;
    CalcTextSize(text, &tw, &th);
    float x = std::floor(rightX - tw);  // Snap to pixel to prevent sub-pixel artifacts
    DrawTextWithShadow(x, y, text, color, shadowColor, shadowOffset);
}

void Renderer::DrawTextScaledRightAligned(float rightX, float y, const char* text, uint32_t color, uint32_t shadowColor,
                                          float scale, float shadowOffset) {
    if (!initialized || !text)
        return;
    float tw = 0, th = 0;
    CalcTextSizeScaled(text, &tw, &th, scale);
    float x = std::floor(rightX - tw);  // Snap to pixel to prevent sub-pixel artifacts
    DrawTextScaledWithShadow(x, y, text, color, shadowColor, scale, shadowOffset);
}

void Renderer::CalcTextSize(const char* text, float* outWidth, float* outHeight) const {
    fontAtlas.CalcTextSize(text, outWidth, outHeight);
}

void Renderer::CalcTextSizeScaled(const char* text, float* outWidth, float* outHeight, float scale) const {
    fontAtlas.CalcTextSize(text, outWidth, outHeight);
    if (outWidth)
        *outWidth *= scale;
    if (outHeight)
        *outHeight *= scale;
}

void Renderer::DrawRect(float x, float y, float w, float h, uint32_t color) {
    // Draw outline as 4 thin rectangles
    float thickness = dpiScale;
    DrawRectFilled(x, y, w, thickness, color);                  // Top
    DrawRectFilled(x, y + h - thickness, w, thickness, color);  // Bottom
    DrawRectFilled(x, y, thickness, h, color);                  // Left
    DrawRectFilled(x + w - thickness, y, thickness, h, color);  // Right
}

void Renderer::DrawRectFilled(float x, float y, float w, float h, uint32_t color) {
    if (!initialized)
        return;

    // Solid-color quads use the diffuse-only backend path on APIs that support it.
    AddQuad(x, y, w, h, 0, 0, 0, 0, color);
    FlushBatch(false);  // Solid color, no texture sampling needed for color
}

void Renderer::DrawLine(float x0, float y0, float x1, float y1, uint32_t color, float thickness) {
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

void Renderer::DrawGraphPolyline(const float* xs, const float* ys, int count, uint32_t color, float thickness) {
    if (count < 2)
        return;

    // Geometry coordinates are already physical backbuffer pixels. Keep the AA
    // fringe exactly one pixel at every Windows DPI setting.
    constexpr float AA_SIZE = 1.0f;
    const float halfThick = thickness * 0.5f;
    const uint32_t colorAA = color & 0x00FFFFFFu;

    struct CrossSection {
        float x, y;
        float nx, ny;
        bool endpoint;
    };
    constexpr int kMaxSections = 2048;
    CrossSection sections[kMaxSections];
    int sectionCount = 0;

    auto segmentNormal = [&](int first, int second, float& nx, float& ny) {
        const float dx = xs[second] - xs[first];
        const float dy = ys[second] - ys[first];
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f) {
            nx = 0.0f;
            ny = 1.0f;
        } else {
            nx = -dy / len;
            ny = dx / len;
        }
    };
    auto addSection = [&](int point, float nx, float ny, bool endpoint) {
        if (sectionCount < kMaxSections) {
            sections[sectionCount++] = {xs[point], ys[point], nx, ny, endpoint};
        }
    };

    constexpr float kMiterLimit = 2.0f;
    for (int i = 0; i < count; ++i) {
        if (i == 0) {
            float nx, ny;
            segmentNormal(0, 1, nx, ny);
            addSection(i, nx, ny, true);
            continue;
        }
        if (i == count - 1) {
            float nx, ny;
            segmentNormal(i - 1, i, nx, ny);
            addSection(i, nx, ny, true);
            continue;
        }

        float n0x, n0y, n1x, n1y;
        segmentNormal(i - 1, i, n0x, n0y);
        segmentNormal(i, i + 1, n1x, n1y);
        float mx = n0x + n1x;
        float my = n0y + n1y;
        const float miterLength = std::sqrt(mx * mx + my * my);
        bool useMiter = false;
        float miterX = n1x;
        float miterY = n1y;
        if (miterLength > 0.001f) {
            mx /= miterLength;
            my /= miterLength;
            const float denominator = mx * n1x + my * n1y;
            if (denominator > 0.001f) {
                const float scale = 1.0f / denominator;
                if (scale <= kMiterLimit) {
                    miterX = mx * scale;
                    miterY = my * scale;
                    useMiter = true;
                }
            }
        }

        if (useMiter) {
            addSection(i, miterX, miterY, false);
        } else {
            // Two coincident cross-sections form a true bevel wedge while still
            // remaining part of the same solid draw command.
            addSection(i, n0x, n0y, false);
            addSection(i, n1x, n1y, false);
        }
    }

    if (sectionCount < 2 || vertices.size() > 0xFFFFu - (size_t)sectionCount * 4u)
        return;

    // Build 4 vertices per point (outer+, inner+, inner-, outer-) so that
    // adjacent segments SHARE the joint vertices instead of duplicating them.
    // This eliminates double-alpha at joints that causes brightness flickering.
    //
    // At endpoints (first/last), the AA fringe is suppressed to prevent the
    // perpendicular normal from extending the fringe inward into the graph,
    // which would make the edges appear thicker than the interior segments.
    const uint16_t firstIdx = (uint16_t)vertices.size();
    for (int i = 0; i < sectionCount; i++) {
        const CrossSection& section = sections[i];
        const float nx = section.nx;
        const float ny = section.ny;
        const bool isEndpoint = section.endpoint;
        if (isEndpoint) {
            // No AA fringe at endpoints - just core vertices (flat cap)
            vertices.push_back({section.x + nx * halfThick, section.y + ny * halfThick, 0, 0, color});
            vertices.push_back({section.x + nx * halfThick, section.y + ny * halfThick, 0, 0, color});
            vertices.push_back({section.x - nx * halfThick, section.y - ny * halfThick, 0, 0, color});
            vertices.push_back({section.x - nx * halfThick, section.y - ny * halfThick, 0, 0, color});
        } else {
            vertices.push_back(
                {section.x + nx * (halfThick + AA_SIZE), section.y + ny * (halfThick + AA_SIZE), 0, 0, colorAA});
            vertices.push_back({section.x + nx * halfThick, section.y + ny * halfThick, 0, 0, color});
            vertices.push_back({section.x - nx * halfThick, section.y - ny * halfThick, 0, 0, color});
            vertices.push_back(
                {section.x - nx * (halfThick + AA_SIZE), section.y - ny * (halfThick + AA_SIZE), 0, 0, colorAA});
        }
    }

    // 3 quads per segment (top fringe, core, bottom fringe) referencing the
    // shared per-point vertices of adjacent points.
    for (int i = 0; i < sectionCount - 1; i++) {
        const uint16_t p0 = firstIdx + (uint16_t)(i * 4);
        const uint16_t p1 = firstIdx + (uint16_t)((i + 1) * 4);

        // Top AA fringe (outer+ → inner+)
        indices.push_back(p0 + 0);
        indices.push_back(p0 + 1);
        indices.push_back(p1 + 1);
        indices.push_back(p0 + 0);
        indices.push_back(p1 + 1);
        indices.push_back(p1 + 0);

        // Core band (inner+ → inner-)
        indices.push_back(p0 + 1);
        indices.push_back(p0 + 2);
        indices.push_back(p1 + 2);
        indices.push_back(p0 + 1);
        indices.push_back(p1 + 2);
        indices.push_back(p1 + 1);

        // Bottom AA fringe (inner- → outer-)
        indices.push_back(p0 + 2);
        indices.push_back(p0 + 3);
        indices.push_back(p1 + 3);
        indices.push_back(p0 + 2);
        indices.push_back(p1 + 3);
        indices.push_back(p1 + 2);
    }

    FlushBatch(false);
}

void Renderer::DrawFrameTimeGraph(float x, float y, float width, float height, const float* frameTimes, int count,
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

    // Interpolate between exact endpoints. Rounding the step made 180 samples
    // overrun narrow panels and then collapse onto the clamped right edge.
    const float edgeInset = (std::min)(2.0f, width * 0.25f);
    const float firstX = x + edgeInset;
    const float lastX = x + width - edgeInset;
    const float stepX = (lastX - firstX) / (float)(count - 1);

    float xs[kMaxPoints], ys[kMaxPoints];
    const float invRange = 1.0f / range;

    for (int i = 0; i < count; i++) {
        float val = frameTimes[i];
        val = (std::max)(minVal, (std::min)(maxVal, val));
        xs[i] = firstX + (float)i * stepX;
        ys[i] = y + height - ((val - minVal) * invRange) * height;
    }

    // Skip leading zero-value samples (unfilled ring buffer at startup).
    int firstValid = 0;
    while (firstValid < count - 1 && frameTimes[firstValid] <= 0.0f)
        firstValid++;
    const int validCount = count - firstValid;
    if (validCount < 2)
        return;

    const float* vxs = xs + firstValid;
    const float* vys = ys + firstValid;

    // Connected polyline with bounded miter/bevel joins + 1px AA fringe.
    // 0.75 logical pixels at every DPI (e.g. 100%→0.75px, 200%→1.5px physical = same visual size).
    const float lineThickness = 0.75f * dpiScale;
    DrawGraphPolyline(vxs, vys, validCount, color, lineThickness);
}

void Renderer::BeginWindow(float x, float y, uint32_t bgColor, float alpha) {
    if (!initialized)
        return;

    windowX = x;
    windowY = y;
    cursorX = x + 8 * dpiScale;  // Padding
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

void Renderer::TableRow(const char* label, const char* value, uint32_t labelColor, uint32_t valueColor) {
    if (!initialized || !inTable)
        return;

    float colWidth = 100.0f * dpiScale;  // Fixed column width for label

    // Draw label
    DrawText(cursorX, cursorY, label, labelColor);

    // Draw value
    DrawText(cursorX + colWidth, cursorY, value, valueColor);

    // Move to next row
    cursorY += static_cast<float>(fontAtlas.GetLineHeight());
}

void Renderer::EndTable() {
    if (!initialized)
        return;
    inTable = false;
}

}  // namespace CustomOverlay
