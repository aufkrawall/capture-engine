#include "overlay_cpu_raster.h"

#include <algorithm>
#include <cmath>

namespace ce::overlay_cpu_raster {

namespace {

struct Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

// The shared draw format stores colour as ABGR, matching what the GPU backends
// upload as D3DCOLOR after their own swap.
Vertex ToVertex(const CustomOverlay::DrawVertex& source, const Target& target) {
    Vertex vertex;
    vertex.x = source.x - static_cast<float>(target.left);
    vertex.y = source.y - static_cast<float>(target.top);
    vertex.u = source.u;
    vertex.v = source.v;
    vertex.r = static_cast<float>(source.color & 0xFFu) / 255.0f;
    vertex.g = static_cast<float>((source.color >> 8) & 0xFFu) / 255.0f;
    vertex.b = static_cast<float>((source.color >> 16) & 0xFFu) / 255.0f;
    vertex.a = static_cast<float>((source.color >> 24) & 0xFFu) / 255.0f;
    return vertex;
}

// Bilinear sample of the RGBA atlas, matching the linear filtering the GPU
// backends ask for so the glyph edges keep the same weight.
void SampleAtlas(const FontAtlasView& atlas, float u, float v, float& outR, float& outG, float& outB, float& outA) {
    outR = outG = outB = outA = 0.0f;
    if (!atlas.pixels || atlas.width <= 0 || atlas.height <= 0)
        return;

    const float x = u * static_cast<float>(atlas.width) - 0.5f;
    const float y = v * static_cast<float>(atlas.height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    auto texel = [&](int px, int py, float& r, float& g, float& b, float& a) {
        px = std::clamp(px, 0, atlas.width - 1);
        py = std::clamp(py, 0, atlas.height - 1);
        const uint8_t* pixel = atlas.pixels + (static_cast<size_t>(py) * atlas.width + px) * 4u;
        r = static_cast<float>(pixel[0]) / 255.0f;
        g = static_cast<float>(pixel[1]) / 255.0f;
        b = static_cast<float>(pixel[2]) / 255.0f;
        a = static_cast<float>(pixel[3]) / 255.0f;
    };

    float r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
    texel(x0, y0, r00, g00, b00, a00);
    texel(x0 + 1, y0, r10, g10, b10, a10);
    texel(x0, y0 + 1, r01, g01, b01, a01);
    texel(x0 + 1, y0 + 1, r11, g11, b11, a11);

    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;

    outR = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
    outG = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
    outB = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
    outA = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
}

uint32_t PackPremultiplied(float r, float g, float b, float a) {
    const auto channel = [](float value) {
        const int scaled = static_cast<int>(std::lround(value * 255.0f));
        return static_cast<uint32_t>(std::clamp(scaled, 0, 255));
    };
    return (channel(a) << 24) | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

void UnpackPremultiplied(uint32_t packed, float& r, float& g, float& b, float& a) {
    b = static_cast<float>(packed & 0xFFu) / 255.0f;
    g = static_cast<float>((packed >> 8) & 0xFFu) / 255.0f;
    r = static_cast<float>((packed >> 16) & 0xFFu) / 255.0f;
    a = static_cast<float>((packed >> 24) & 0xFFu) / 255.0f;
}

// One triangle, source-over into premultiplied storage.
//
// The fill rule matters as much as the coverage: `AddQuad` emits two triangles
// sharing a diagonal, so a pixel exactly on that diagonal belongs to both
// unless the rule excludes it from one. Without it every such pixel is blended
// twice and a translucent overlay comes out patchy along its own diagonals.
void RasterizeTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2, bool useTexture,
                       const FontAtlasView& atlas, const Target& target, std::vector<uint32_t>& out) {
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (std::fabs(area) < 1e-6f)
        return;

    // Normalize the winding so one sign test covers every edge.
    const Vertex& a = v0;
    const Vertex& b = area < 0.0f ? v2 : v1;
    const Vertex& c = area < 0.0f ? v1 : v2;
    area = std::fabs(area);
    const float inverseArea = 1.0f / area;

    int minX = static_cast<int>(std::floor((std::min)({a.x, b.x, c.x})));
    int maxX = static_cast<int>(std::ceil((std::max)({a.x, b.x, c.x})));
    int minY = static_cast<int>(std::floor((std::min)({a.y, b.y, c.y})));
    int maxY = static_cast<int>(std::ceil((std::max)({a.y, b.y, c.y})));
    minX = (std::max)(minX, 0);
    minY = (std::max)(minY, 0);
    maxX = (std::min)(maxX, target.width - 1);
    maxY = (std::min)(maxY, target.height - 1);
    if (minX > maxX || minY > maxY)
        return;

    // A pixel exactly on an edge belongs to the triangle only when that edge is
    // the shape's top or left one, so two triangles sharing it cover it once.
    const auto isTopLeft = [](const Vertex& from, const Vertex& to) {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        return (dy == 0.0f && dx < 0.0f) || dy > 0.0f;
    };
    const bool topLeftAB = isTopLeft(a, b);
    const bool topLeftBC = isTopLeft(b, c);
    const bool topLeftCA = isTopLeft(c, a);

    // The edge functions are affine, so each is stepped along the scanline
    // rather than solved per pixel. The overlay is mostly one large background
    // rectangle, so this is the loop that decides whether the rasterizer can
    // sit on the presentation path at all.
    const float abStepX = -(b.y - a.y);
    const float bcStepX = -(c.y - b.y);
    const float caStepX = -(a.y - c.y);

    for (int y = minY; y <= maxY; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float px0 = static_cast<float>(minX) + 0.5f;
        float ab = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px0 - a.x);
        float bc = (c.x - b.x) * (py - b.y) - (c.y - b.y) * (px0 - b.x);
        float ca = (a.x - c.x) * (py - c.y) - (a.y - c.y) * (px0 - c.x);
        uint32_t* row = out.data() + static_cast<size_t>(y) * target.width;

        for (int x = minX; x <= maxX; ++x, ab += abStepX, bc += bcStepX, ca += caStepX) {
            const bool insideAB = ab > 0.0f || (ab == 0.0f && topLeftAB);
            const bool insideBC = bc > 0.0f || (bc == 0.0f && topLeftBC);
            const bool insideCA = ca > 0.0f || (ca == 0.0f && topLeftCA);
            if (!insideAB || !insideBC || !insideCA)
                continue;

            // bc weights a, ca weights b, ab weights c.
            const float wa = bc * inverseArea;
            const float wb = ca * inverseArea;
            const float wc = ab * inverseArea;

            float r = a.r * wa + b.r * wb + c.r * wc;
            float g = a.g * wa + b.g * wb + c.g * wc;
            float bl = a.b * wa + b.b * wb + c.b * wc;
            float al = a.a * wa + b.a * wb + c.a * wc;

            if (useTexture) {
                const float u = a.u * wa + b.u * wb + c.u * wc;
                const float v = a.v * wa + b.v * wb + c.v * wc;
                float tr, tg, tb, ta;
                SampleAtlas(atlas, u, v, tr, tg, tb, ta);
                // Modulate, matching the fixed-function stage the GPU backends set.
                r *= tr;
                g *= tg;
                bl *= tb;
                al *= ta;
            }

            if (al <= 0.0f)
                continue;

            float dr, dg, db, da;
            UnpackPremultiplied(row[x], dr, dg, db, da);
            const float inverse = 1.0f - al;
            row[x] = PackPremultiplied(r * al + dr * inverse, g * al + dg * inverse, bl * al + db * inverse,
                                       al + da * inverse);
        }
    }
}

}  // namespace

bool Rasterize(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
               const std::vector<CustomOverlay::DrawCommand>& commands, const FontAtlasView& atlas,
               const Target& target, std::vector<uint32_t>& out) {
    if (target.width <= 0 || target.height <= 0 || vertices.empty() || indices.empty() || commands.empty())
        return false;

    out.assign(static_cast<size_t>(target.width) * static_cast<size_t>(target.height), 0u);

    bool drewAnything = false;
    for (const CustomOverlay::DrawCommand& command : commands) {
        if (command.indexCount == 0)
            continue;
        const size_t first = command.indexOffset;
        const size_t last = first + command.indexCount;
        if (last > indices.size())
            continue;

        for (size_t i = first; i + 2 < last; i += 3) {
            const uint16_t i0 = indices[i];
            const uint16_t i1 = indices[i + 1];
            const uint16_t i2 = indices[i + 2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                continue;
            RasterizeTriangle(ToVertex(vertices[i0], target), ToVertex(vertices[i1], target),
                              ToVertex(vertices[i2], target), command.useTexture, atlas, target, out);
            drewAnything = true;
        }
    }
    return drewAnything;
}

}  // namespace ce::overlay_cpu_raster
