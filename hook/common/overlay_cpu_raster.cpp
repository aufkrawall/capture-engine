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

// Packed output layout: 0xAARRGGBB, i.e. BGRA bytes in memory.
inline uint32_t PackPremultiplied8(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

inline uint32_t Mul255(uint32_t a, uint32_t b) {
    return (a * b + 127u) / 255u;
}

// One premultiplied source-over step, entirely in integer arithmetic. This is
// the operation every quad and triangle ends in, and the panel background runs
// it a few hundred thousand times per full rasterization; the float
// unpack/blend/repack the first version used was the largest single cost in the
// DirectDraw composite.
inline uint32_t SourceOverPremultiplied(uint32_t source, uint32_t destination) {
    const uint32_t alpha = (source >> 24) & 0xFFu;
    if (alpha == 0xFFu)
        return source | 0xFF000000u;
    if (alpha == 0u)
        return destination;
    const uint32_t inverse = 255u - alpha;
    const uint32_t blue = (source & 0xFFu) + Mul255(destination & 0xFFu, inverse);
    const uint32_t green = ((source >> 8) & 0xFFu) + Mul255((destination >> 8) & 0xFFu, inverse);
    const uint32_t red = ((source >> 16) & 0xFFu) + Mul255((destination >> 16) & 0xFFu, inverse);
    // The sprite accumulates over a transparent destination, so the result's
    // alpha is part of the answer: opaque output is only correct when the
    // destination was opaque too.
    const uint32_t outAlpha = alpha + Mul255((destination >> 24) & 0xFFu, inverse);
    const uint32_t clampedBlue = blue > 255u ? 255u : blue;
    const uint32_t clampedGreen = green > 255u ? 255u : green;
    const uint32_t clampedRed = red > 255u ? 255u : red;
    const uint32_t clampedAlpha = outAlpha > 255u ? 255u : outAlpha;
    return PackPremultiplied8(clampedRed, clampedGreen, clampedBlue, clampedAlpha);
}

// Premultiplies a straight packed ABGR colour. ABGR here is 0xAABBGGRR, the
// same byte order D3DCOLOR presents as BGRA in memory, so red is the low byte.
inline uint32_t Premultiply(uint32_t abgr) {
    const uint32_t alpha = (abgr >> 24) & 0xFFu;
    return PackPremultiplied8(Mul255(abgr & 0xFFu, alpha), Mul255((abgr >> 8) & 0xFFu, alpha),
                              Mul255((abgr >> 16) & 0xFFu, alpha), alpha);
}

uint32_t PackPremultiplied(float r, float g, float b, float a) {
    const auto channel = [](float value) {
        const long scaled = std::lround(value * 255.0f);
        return static_cast<uint32_t>(std::clamp(scaled, 0L, 255L));
    };
    return PackPremultiplied8(channel(r), channel(g), channel(b), channel(a));
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

// The generic triangle path. Everything the overlay's line renderer emits that
// is not an axis-aligned rectangle lands here; rectangles and glyphs take the
// much shorter quad path below.
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
    // rather than solved per pixel.
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

            row[x] = SourceOverPremultiplied(PackPremultiplied(r * al, g * al, bl * al, al), row[x]);
        }
    }
}

// The renderer emits every rectangle and every glyph as an axis-aligned quad
// through AddQuad, whose six indices always name four such corners. Filling the
// rectangle directly skips the edge functions, the per-pixel interpolation and
// the top-left rule for the shapes that cover most of the overlay's pixels.
bool TryRasterizeAxisAlignedQuad(const uint32_t (&colors)[4], bool useTexture, const FontAtlasView& atlas,
                                 const Vertex& v0, const Vertex& v1, const Vertex& v2, const Vertex& v3,
                                 const Target& target, std::vector<uint32_t>& out) {
    // Corners must be shared exactly; a rotated graph segment falls through.
    if (std::fabs(v0.y - v1.y) > 0.01f || std::fabs(v2.y - v3.y) > 0.01f || std::fabs(v0.x - v3.x) > 0.01f ||
        std::fabs(v1.x - v2.x) > 0.01f || v1.x <= v0.x || v2.y <= v0.y) {
        return false;
    }
    // A vertex colour that differs between the four corners has to interpolate,
    // which only the triangle path does; the shared renderer never emits it.
    if (colors[0] != colors[1] || colors[0] != colors[2] || colors[0] != colors[3])
        return false;

    // Match triangle rasterization's pixel-centre/top-left coverage. Using
    // floor(left)..ceil(right) paints an extra column whenever a DPI-scaled
    // edge lies in the second half of a pixel, which makes the fast quad path
    // visibly wider than the native GPU path.
    int minX = static_cast<int>(std::ceil(v0.x - 0.5f));
    int maxX = static_cast<int>(std::ceil(v1.x - 0.5f));
    int minY = static_cast<int>(std::ceil(v0.y - 0.5f));
    int maxY = static_cast<int>(std::ceil(v2.y - 0.5f));
    minX = (std::max)(minX, 0);
    minY = (std::max)(minY, 0);
    maxX = (std::min)(maxX, target.width);
    maxY = (std::min)(maxY, target.height);
    if (minX >= maxX || minY >= maxY)
        return true;  // The quad exists, it just has no pixels inside the target.

    if (!useTexture) {
        const uint32_t source = Premultiply(colors[0]);
        const uint32_t alpha = (source >> 24) & 0xFFu;
        if (alpha == 0u)
            return true;
        for (int y = minY; y < maxY; ++y) {
            uint32_t* row = out.data() + static_cast<size_t>(y) * target.width;
            if (alpha == 0xFFu) {
                for (int x = minX; x < maxX; ++x)
                    row[x] = source | 0xFF000000u;
            } else {
                for (int x = minX; x < maxX; ++x)
                    row[x] = SourceOverPremultiplied(source, row[x]);
            }
        }
        return true;
    }

    if (!atlas.pixels || atlas.width <= 0 || atlas.height <= 0)
        return true;

    const float width = v1.x - v0.x;
    const float height = v2.y - v0.y;
    const float uPerPixel = (v1.u - v0.u) / width;
    const float vPerPixel = (v3.v - v0.v) / height;

    for (int y = minY; y < maxY; ++y) {
        const float v = v0.v + (static_cast<float>(y) + 0.5f - v0.y) * vPerPixel;
        uint32_t* row = out.data() + static_cast<size_t>(y) * target.width;
        for (int x = minX; x < maxX; ++x) {
            const float u = v0.u + (static_cast<float>(x) + 0.5f - v0.x) * uPerPixel;
            float tr, tg, tb, ta;
            SampleAtlas(atlas, u, v, tr, tg, tb, ta);
            const float alpha = v0.a * ta;
            if (alpha <= 0.0f)
                continue;
            row[x] = SourceOverPremultiplied(
                PackPremultiplied(v0.r * tr * alpha, v0.g * tg * alpha, v0.b * tb * alpha, alpha), row[x]);
        }
    }
    return true;
}

// Draws one command. Returns true when at least one of its quads or triangles
// was submitted to the rasterizer (even if it clipped away).
bool DrawCommand(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                 const CustomOverlay::DrawCommand& command, const FontAtlasView& atlas, const Target& target,
                 std::vector<uint32_t>& out) {
    if (command.indexCount == 0)
        return false;
    const size_t first = command.indexOffset;
    const size_t last = first + command.indexCount;
    if (last > indices.size())
        return false;

    bool drewAnything = false;
    size_t i = first;
    for (; i + 5 < last; i += 6) {
        const uint16_t i0 = indices[i];
        const uint16_t i1 = indices[i + 1];
        const uint16_t i2 = indices[i + 2];
        const uint16_t i3 = indices[i + 5];
        // AddQuad's pattern: (0,1,2,0,2,3). Anything else is triangle soup and
        // is handled index by index below.
        if (i0 != indices[i + 3] || i2 != indices[i + 4] || i0 == i1 || i1 == i2 || i2 == i3 || i0 == i2 ||
            i0 == i3 || i1 == i3 || i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size() ||
            i3 >= vertices.size()) {
            break;
        }
        const Vertex q0 = ToVertex(vertices[i0], target);
        const Vertex q1 = ToVertex(vertices[i1], target);
        const Vertex q2 = ToVertex(vertices[i2], target);
        const Vertex q3 = ToVertex(vertices[i3], target);
        const uint32_t colors[4] = {vertices[i0].color, vertices[i1].color, vertices[i2].color, vertices[i3].color};
        if (!TryRasterizeAxisAlignedQuad(colors, command.useTexture, atlas, q0, q1, q2, q3, target, out)) {
            RasterizeTriangle(q0, q1, q2, command.useTexture, atlas, target, out);
            RasterizeTriangle(q0, q2, q3, command.useTexture, atlas, target, out);
        }
        drewAnything = true;
    }
    for (; i + 2 < last; i += 3) {
        const uint16_t i0 = indices[i];
        const uint16_t i1 = indices[i + 1];
        const uint16_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
            continue;
        RasterizeTriangle(ToVertex(vertices[i0], target), ToVertex(vertices[i1], target),
                          ToVertex(vertices[i2], target), command.useTexture, atlas, target, out);
        drewAnything = true;
    }
    return drewAnything;
}

bool DrawCommandRange(const std::vector<CustomOverlay::DrawVertex>& vertices,
                      const std::vector<uint16_t>& indices,
                      const std::vector<CustomOverlay::DrawCommand>& commands, const FontAtlasView& atlas,
                      const Target& target, size_t firstCommand, size_t lastCommand, std::vector<uint32_t>& out) {
    bool drewAnything = false;
    for (size_t c = firstCommand; c < lastCommand && c < commands.size(); ++c) {
        drewAnything = DrawCommand(vertices, indices, commands[c], atlas, target, out) || drewAnything;
    }
    return drewAnything;
}

// Bounding box of the pixels a primitive can touch, in target coordinates. The
// half pixel of slack covers the anti-aliased fringe a rotated segment writes
// outside its own vertices.
PixelRect PrimitiveBounds(const PrimitiveSnapshot& primitive, const Target& target) {
    PixelRect bounds;
    if (primitive.vertexCount < 3 || primitive.vertexCount > primitive.vertices.size())
        return bounds;
    float minX = primitive.vertices[0].x - static_cast<float>(target.left);
    float minY = primitive.vertices[0].y - static_cast<float>(target.top);
    float maxX = minX;
    float maxY = minY;
    for (size_t i = 1; i < primitive.vertexCount; ++i) {
        const float x = primitive.vertices[i].x - static_cast<float>(target.left);
        const float y = primitive.vertices[i].y - static_cast<float>(target.top);
        minX = (std::min)(minX, x);
        minY = (std::min)(minY, y);
        maxX = (std::max)(maxX, x);
        maxY = (std::max)(maxY, y);
    }
    bounds.left = (std::max)(static_cast<int>(std::floor(minX)) - 1, 0);
    bounds.top = (std::max)(static_cast<int>(std::floor(minY)) - 1, 0);
    bounds.right = (std::min)(static_cast<int>(std::ceil(maxX)) + 1, target.width);
    bounds.bottom = (std::min)(static_cast<int>(std::ceil(maxY)) + 1, target.height);
    return bounds;
}

PixelRect UnionRects(const PixelRect& a, const PixelRect& b) {
    if (a.IsEmpty())
        return b;
    if (b.IsEmpty())
        return a;
    PixelRect merged;
    merged.left = (std::min)(a.left, b.left);
    merged.top = (std::min)(a.top, b.top);
    merged.right = (std::max)(a.right, b.right);
    merged.bottom = (std::max)(a.bottom, b.bottom);
    return merged;
}

bool SameTarget(const Target& a, const Target& b) {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}

bool SameVertex(const CustomOverlay::DrawVertex& a, const CustomOverlay::DrawVertex& b) {
    return a.x == b.x && a.y == b.y && a.u == b.u && a.v == b.v && a.color == b.color;
}

bool SamePrimitive(const PrimitiveSnapshot& a, const PrimitiveSnapshot& b) {
    if (a.vertexCount != b.vertexCount || a.useTexture != b.useTexture)
        return false;
    for (size_t i = 0; i < a.vertexCount; ++i) {
        if (!SameVertex(a.vertices[i], b.vertices[i]))
            return false;
    }
    return true;
}

void AppendPrimitive(std::vector<PrimitiveSnapshot>& out, const std::vector<CustomOverlay::DrawVertex>& vertices,
                     const Target& target, bool useTexture, const uint16_t* indices, uint8_t vertexCount) {
    PrimitiveSnapshot primitive;
    primitive.vertexCount = vertexCount;
    primitive.useTexture = useTexture;
    for (size_t i = 0; i < vertexCount; ++i)
        primitive.vertices[i] = vertices[indices[i]];
    primitive.bounds = PrimitiveBounds(primitive, target);
    out.push_back(primitive);
}

// Mirrors DrawCommand's quad recognition exactly. Keeping the cache unit at
// primitive granularity is what allows a moving graph to share a renderer batch
// with the stable panel without invalidating the whole panel.
void BuildPrimitiveSnapshots(const std::vector<CustomOverlay::DrawVertex>& vertices,
                             const std::vector<uint16_t>& indices,
                             const std::vector<CustomOverlay::DrawCommand>& commands, const Target& target,
                             std::vector<PrimitiveSnapshot>& out) {
    out.clear();
    out.reserve(indices.size() / 3);
    for (const auto& command : commands) {
        const size_t first = command.indexOffset;
        const size_t last = first + command.indexCount;
        if (command.indexCount == 0 || last > indices.size())
            continue;
        size_t i = first;
        for (; i + 5 < last; i += 6) {
            const uint16_t quad[4] = {indices[i], indices[i + 1], indices[i + 2], indices[i + 5]};
            if (quad[0] != indices[i + 3] || quad[2] != indices[i + 4] || quad[0] == quad[1] ||
                quad[1] == quad[2] || quad[2] == quad[3] || quad[0] == quad[2] || quad[0] == quad[3] ||
                quad[1] == quad[3] || quad[0] >= vertices.size() || quad[1] >= vertices.size() ||
                quad[2] >= vertices.size() || quad[3] >= vertices.size()) {
                break;
            }
            AppendPrimitive(out, vertices, target, command.useTexture, quad, 4);
        }
        for (; i + 2 < last; i += 3) {
            const uint16_t triangle[3] = {indices[i], indices[i + 1], indices[i + 2]};
            if (triangle[0] >= vertices.size() || triangle[1] >= vertices.size() ||
                triangle[2] >= vertices.size()) {
                continue;
            }
            AppendPrimitive(out, vertices, target, command.useTexture, triangle, 3);
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
    return DrawCommandRange(vertices, indices, commands, atlas, target, 0, commands.size(), out);
}

bool UpdateCommandCache(CommandCache& cache, const std::vector<CustomOverlay::DrawVertex>& vertices,
                        const std::vector<uint16_t>& indices,
                        const std::vector<CustomOverlay::DrawCommand>& commands, const FontAtlasView& atlas,
                        const Target& target, RasterStats& stats, PixelRect& changedBounds) {
    changedBounds = {};
    if (target.width <= 0 || target.height <= 0 || vertices.empty() || indices.empty() || commands.empty())
        return false;

    auto& current = cache.pendingPrimitives;
    BuildPrimitiveSnapshots(vertices, indices, commands, target, current);
    if (current.empty())
        return false;

    const size_t pixelCount = static_cast<size_t>(target.width) * static_cast<size_t>(target.height);
    const bool sameTarget = cache.hasFrame && SameTarget(cache.target, target) &&
                            cache.composed.size() == pixelCount && cache.atlasPixels == atlas.pixels &&
                            cache.atlasWidth == atlas.width && cache.atlasHeight == atlas.height;

    if (!sameTarget) {
        if (!Rasterize(vertices, indices, commands, atlas, target, cache.composed))
            return false;
        cache.target = target;
        cache.atlasPixels = atlas.pixels;
        cache.atlasWidth = atlas.width;
        cache.atlasHeight = atlas.height;
        cache.primitives.swap(current);
        cache.hasFrame = true;
        ++stats.fullRasters;
        stats.primitiveRenders += static_cast<uint32_t>(cache.primitives.size());
        changedBounds.left = 0;
        changedBounds.top = 0;
        changedBounds.right = target.width;
        changedBounds.bottom = target.height;
        return true;
    }

    size_t prefix = 0;
    const size_t commonCount = (std::min)(cache.primitives.size(), current.size());
    while (prefix < commonCount && SamePrimitive(cache.primitives[prefix], current[prefix]))
        ++prefix;

    if (prefix == cache.primitives.size() && prefix == current.size()) {
        ++stats.spriteReuses;
        stats.primitiveReuses += static_cast<uint32_t>(prefix);
        return true;
    }

    size_t suffix = 0;
    while (suffix < commonCount - prefix &&
           SamePrimitive(cache.primitives[cache.primitives.size() - 1 - suffix],
                         current[current.size() - 1 - suffix])) {
        ++suffix;
    }

    PixelRect dirty;
    for (size_t i = prefix; i < cache.primitives.size() - suffix; ++i)
        dirty = UnionRects(dirty, cache.primitives[i].bounds);
    for (size_t i = prefix; i < current.size() - suffix; ++i)
        dirty = UnionRects(dirty, current[i].bounds);

    stats.primitiveChanges += static_cast<uint32_t>((cache.primitives.size() - prefix - suffix) +
                                                    (current.size() - prefix - suffix));
    stats.primitiveReuses += static_cast<uint32_t>(prefix + suffix);
    cache.primitives.swap(current);

    // Changes wholly outside the target cannot alter its pixels.
    if (dirty.IsEmpty())
        return true;

    Target dirtyTarget;
    dirtyTarget.left = target.left + dirty.left;
    dirtyTarget.top = target.top + dirty.top;
    dirtyTarget.width = dirty.right - dirty.left;
    dirtyTarget.height = dirty.bottom - dirty.top;
    if (!Rasterize(vertices, indices, commands, atlas, dirtyTarget, cache.scratch))
        return false;

    for (int y = 0; y < dirtyTarget.height; ++y) {
        const uint32_t* source = cache.scratch.data() + static_cast<size_t>(y) * dirtyTarget.width;
        uint32_t* destination = cache.composed.data() + static_cast<size_t>(dirty.top + y) * target.width + dirty.left;
        std::copy_n(source, dirtyTarget.width, destination);
    }
    ++stats.dirtyRasters;
    stats.primitiveRenders += static_cast<uint32_t>(cache.primitives.size());
    changedBounds = dirty;
    return true;
}

}  // namespace ce::overlay_cpu_raster
