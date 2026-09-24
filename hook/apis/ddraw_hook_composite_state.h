/**
 * State for the DirectDraw CPU composite.
 *
 * The composite is a read-modify-write into a surface the application also
 * draws into, so it needs two things the presentation hooks alone cannot give
 * it: the application's pixels under CE's rectangle, and the exact bytes CE
 * last left there. The second is what lets a repeated presentation recognize
 * its own output (`read == lastComposite`) and restore the backdrop instead of
 * blending the translucent overlay over itself a second time.
 *
 * One entry is kept per surface a presentation has published into. A flip
 * chain alternates its buffers, so a single entry would discard the backdrop of
 * the buffer it is about to serve next and take CE's own output for the
 * application's frame.
 */

#pragma once

#include <unordered_map>

#include "ddraw_hook_internal.h"
#include "../common/overlay_cpu_raster.h"

// Eight entries cover the configured maximum six-image DirectDraw flip chain
// with room for alternate blit targets; the least recently used entry is
// dropped when a game presents through more than that.
struct DDrawCapture::DDrawCompositeState {
    struct SurfaceState {
        uintptr_t identity = 0;
        ce::ddraw_present_policy::Rect region = {};
        // Canonical 0xAARRGGBB (BGRA in memory), opaque. `backdrop` is the
        // application's image under CE's rectangle; `lastComposite` is what CE
        // wrote into that rectangle.
        std::vector<uint32_t> backdrop;
        std::vector<uint32_t> lastComposite;
        bool valid = false;
        uint32_t lastUse = 0;
    };

    static constexpr size_t kMaxSurfaceStates = 8;

    DDrawCompositeState() {
        surfaces.reserve(kMaxSurfaceStates);
    }

    ce::overlay_cpu_raster::CommandCache spriteCache;
    std::vector<SurfaceState> surfaces;
    // One row of the dirty rectangle, in ordinary cached memory. The composite
    // copies a row out of the locked surface, computes over it here and copies
    // it back, so neither video-memory stream is ever accessed pixel by pixel.
    // A 16-bit surface needs the packed row as well: it is copied whole, then
    // expanded into and repacked out of `rowScratch`. 8-bit and 24-bit rows use
    // `byteRowScratch` the same way (one and three bytes per pixel).
    std::vector<uint32_t> rowScratch;
    std::vector<uint16_t> packedRowScratch;
    std::vector<uint8_t> byteRowScratch;
    // Palette quantization memo for one composite pass: blended overlay colours
    // repeat heavily and searching 256 entries per pixel would run on the
    // application's render thread. The snapshot it answers from is refetched
    // every pass, so the memo never outlives the palette it was built against.
    std::unordered_map<uint32_t, uint8_t> paletteQuantizeCache;
    uint32_t useCounter = 0;
};
