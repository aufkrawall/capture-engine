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
    uint32_t useCounter = 0;
};
