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
 */

#pragma once

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

// The font atlas as the shared renderer holds it: tightly packed RGBA.
struct FontAtlasView {
    const uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
};

// Rasterizes the draw list into `out`, which is resized to width*height pixels
// and cleared to fully transparent first. False means nothing was drawn.
bool Rasterize(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
               const std::vector<CustomOverlay::DrawCommand>& commands, const FontAtlasView& atlas,
               const Target& target, std::vector<uint32_t>& out);

}  // namespace ce::overlay_cpu_raster
