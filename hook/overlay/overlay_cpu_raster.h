/**
 * CPU rasterizer for the shared overlay draw list.
 *
 * The DirectDraw/DX6/DX7 compatibility route has no GPU path into a DirectDraw
 * surface. Producing the overlay's pixels on the GPU therefore costs a readback
 * - and `GetRenderTargetData` blocks the render thread until the GPU has caught
 * up - on *every* presentation, on a surface the display is scanning out. The
 * application draws, the overlay is missing for as long as that round trip
 * takes, and then it reappears: Gothic II's intro videos and loading screens
 * update the scanout surface about ten times a second and strobe exactly that
 * way (sessions 20260914_190240 and 20260914_193129, where every composite
 * succeeded and none of the values were flapping).
 *
 * The draw list is small - on the order of a thousand transformed vertices, a
 * handful of commands and a GDI-rasterized font atlas - and entirely CPU-side
 * already. Rasterizing it here removes the GPU, the readback and the
 * synchronization from that route completely.
 *
 * Cost is not an afterthought. The overlay is redrawn on every presentation
 * because the frame-time graph advances, but most of its primitives are
 * byte-identical between presentations. The renderer deliberately merges
 * adjacent solid geometry into one command, so a command is much too coarse a
 * cache unit: the stable panel and moving graph commonly share one. The cache
 * compares individual quads/triangles, clears only the union of the primitives
 * that changed or disappeared, and redraws the complete list clipped to that
 * rectangle. Axis-aligned quads, which is every rectangle and every glyph,
 * also take an integer fill path instead of the generic triangle path.
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "custom_overlay.h"

namespace ce::overlay_cpu_raster {

// Destination rectangle in viewport pixels. The output buffer is tightly packed
// BGRA, width*height pixels, with colour premultiplied by coverage so it can be
// blended anywhere with one source-over step.
struct Target {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

// A half-open pixel rectangle. The composite only has to move pixels a command
// actually changed, so every cache update reports one of these.
struct PixelRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool IsEmpty() const {
        return right <= left || bottom <= top;
    }
};

// The font atlas as the shared renderer holds it: tightly packed RGBA.
struct FontAtlasView {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
};

// What one cache update did, for the DirectDraw presentation mix line. Without
// these counters a full rebuild and an incremental update are indistinguishable
// from the outside, and the whole point of the cache is which one happened.
struct RasterStats {
    uint32_t fullRasters = 0;
    uint32_t dirtyRasters = 0;
    uint32_t primitiveRenders = 0;
    uint32_t primitiveChanges = 0;
    uint32_t primitiveReuses = 0;
    uint32_t spriteReuses = 0;
};

// Rasterizes the draw list into `out`, which is resized to width*height pixels
// and cleared to fully transparent first. False means nothing was drawn.
bool Rasterize(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
               const std::vector<CustomOverlay::DrawCommand>& commands, const FontAtlasView& atlas,
               const Target& target, std::vector<uint32_t>& out);

// Exact render inputs for one quad or triangle. Geometry is stored instead of a
// hash so a cache hit is proof of pixel equivalence, not a probabilistic match.
struct PrimitiveSnapshot {
    std::array<CustomOverlay::DrawVertex, 4> vertices = {};
    PixelRect bounds = {};
    uint8_t vertexCount = 0;
    bool useTexture = false;
};

// Incremental raster cache for one target rectangle. `composed` is always the
// complete frame after an update; `scratch` is the tightly packed dirty region
// used while repainting changed primitives.
struct CommandCache {
    Target target = {};
    std::vector<uint32_t> composed;
    std::vector<uint32_t> scratch;
    std::vector<PrimitiveSnapshot> primitives;
    // Reused construction storage avoids allocating and freeing a full
    // primitive list on every CPU-composited presentation.
    std::vector<PrimitiveSnapshot> pendingPrimitives;
    const uint8_t* atlasPixels = nullptr;
    int atlasWidth = 0;
    int atlasHeight = 0;
    bool hasFrame = false;
};

// Repaints `cache.composed` from the draw list. `changedBounds` is empty when
// every primitive is byte-identical to the cached one and the caller can keep
// the pixels it already put on the surface. Returns false only when the draw
// list has nothing to draw; an unchanged frame returns true with no bounds.
bool UpdateCommandCache(CommandCache& cache, const std::vector<CustomOverlay::DrawVertex>& vertices,
                        const std::vector<uint16_t>& indices,
                        const std::vector<CustomOverlay::DrawCommand>& commands, const FontAtlasView& atlas,
                        const Target& target, RasterStats& stats, PixelRect& changedBounds);

}  // namespace ce::overlay_cpu_raster
