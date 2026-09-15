#pragma once

#include <cstdint>

// Where a DirectDraw overlay composite belongs.
//
// DirectDraw has no single present entry point: an application publishes a
// frame by flipping a chain, by blitting an offscreen image onto a
// single-buffered primary, or by drawing straight into the primary. The overlay
// has to be inside the image each of those publishes, and that image is decided
// *before* the call reaches the runtime.
//
// Compositing after the call - reading the surface that is on screen, drawing
// the overlay over it and writing the whole thing back - is never correct on a
// flip chain. The write lands in a buffer the display is already scanning out,
// so it tears in, and the next flip replaces that buffer with one the overlay
// never touched. Gothic II (DirectDraw7 + Direct3D7 under the SystemPack,
// session 20260914_173658) showed exactly that signature: an overlay written to
// the primary after every Flip, flickering at the frame rate.
namespace ce::ddraw_present_policy {

struct Extent {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool IsEmpty() const {
        return right <= left || bottom <= top;
    }
};

// What a hooked DirectDraw surface call means for presentation.
enum class PresentKind {
    // Not a presentation. Leave the call alone.
    None,
    // Flip publishes the flip chain's back buffer.
    FlipChain,
    // A full-surface blit onto a single-buffered scanout surface publishes its source.
    BlitPresent,
    // The application changed the scanout surface itself, so the change is already visible.
    DirectScanout,
};

// Which surface the overlay must be composited into.
enum class CompositeTarget {
    None,
    // The image this present is about to publish: the flip target, or the blit source.
    PresentSource,
    // The surface that is already visible, because nothing else is going to publish it.
    VisibleSurface,
};

struct BlitGeometry {
    // The destination is the surface the display is scanning out: writing it is
    // immediately visible, whether or not it heads a flip chain.
    bool destIsScanout = false;
    // The destination is a back buffer. Flip - not this blit - decides when its
    // contents reach the screen, so a write into it is ordinary drawing.
    bool destIsBackBuffer = false;
    // The scanout surface heads a flip chain. Its blit sources are not
    // necessarily frame buffers (a loading screen blits a static image), so the
    // overlay must not be stamped into them; the visible surface is composited
    // after the blit instead.
    bool destOwnsFlipChain = false;
    bool haveSource = false;
    Extent dest;
    Extent source;
    bool haveDestRect = false;
    Rect destRect;
    bool haveSourceRect = false;
    Rect sourceRect;
};

inline bool RectsIntersect(const Rect& a, const Rect& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// The smallest rectangle covering both inputs. An empty rectangle is the
// identity, which is what a dirty region accumulator starts from.
inline Rect UnionRect(const Rect& a, const Rect& b) {
    if (a.IsEmpty())
        return b;
    if (b.IsEmpty())
        return a;
    Rect merged;
    merged.left = a.left < b.left ? a.left : b.left;
    merged.top = a.top < b.top ? a.top : b.top;
    merged.right = a.right > b.right ? a.right : b.right;
    merged.bottom = a.bottom > b.bottom ? a.bottom : b.bottom;
    return merged;
}

// The overlap of two rectangles; empty when they do not intersect.
inline Rect IntersectRect(const Rect& a, const Rect& b) {
    Rect clipped;
    if (!RectsIntersect(a, b))
        return clipped;
    clipped.left = a.left > b.left ? a.left : b.left;
    clipped.top = a.top > b.top ? a.top : b.top;
    clipped.right = a.right < b.right ? a.right : b.right;
    clipped.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    return clipped;
}

// The DirectDraw bootstrap creates a synthetic DirectDraw object (and, through
// the Windows DDraw implementation, a Direct3D device) on CE's worker thread.
// With a co-resident third-party overlay that has already hooked
// Direct3DCreate9, that is the BioShock Infinite crash family. Module presence
// alone is not evidence the application renders through the higher-level API -
// ddraw.dll, d3d9.dll and d3d8.dll are routinely loaded as transitive
// dependencies, and Gothic II session 20260914_195422 is a DirectDraw7 title
// whose process also contains d3d9.dll. Only a created device, or a module
// load with no proof that DirectDraw is actually in use, suppresses the
// bootstrap.
inline bool ShouldSkipDirectDrawBootstrap(bool higherLevelDeviceCreated, bool higherLevelModuleLoaded,
                                          bool directDrawEvidence) {
    if (higherLevelDeviceCreated)
        return true;
    if (!higherLevelModuleLoaded)
        return false;
    return !directDrawEvidence;
}

// A blit is a presentation only when it replaces the whole visible surface from
// another surface of the same size. Everything narrower is a 2D update - a HUD
// piece, a cursor, a video rectangle - and a game can issue dozens of those per
// frame; treating each one as a present re-composites the overlay that many
// times and still misses the frame that follows.
inline PresentKind ClassifyBlit(const BlitGeometry& geometry) {
    // A back buffer is published by Flip, so drawing into one is not a present.
    if (geometry.destIsBackBuffer)
        return PresentKind::None;
    if (!geometry.destIsScanout)
        return PresentKind::None;
    if (geometry.dest.width == 0 || geometry.dest.height == 0)
        return PresentKind::None;
    if (!geometry.haveSource)
        return PresentKind::DirectScanout;
    // On a flip chain the blit still reaches the screen - Gothic II draws its
    // loading screens this way while the 3D scene is not running - but the
    // overlay goes into the visible surface afterwards rather than into a
    // source the application may blit again unchanged.
    if (geometry.destOwnsFlipChain)
        return PresentKind::DirectScanout;

    const bool coversDest = !geometry.haveDestRect ||
                            (geometry.destRect.left == 0 && geometry.destRect.top == 0 &&
                             geometry.destRect.right == static_cast<int>(geometry.dest.width) &&
                             geometry.destRect.bottom == static_cast<int>(geometry.dest.height));
    const bool coversSource = !geometry.haveSourceRect ||
                              (geometry.sourceRect.left == 0 && geometry.sourceRect.top == 0 &&
                               geometry.sourceRect.right == static_cast<int>(geometry.source.width) &&
                               geometry.sourceRect.bottom == static_cast<int>(geometry.source.height));
    const bool sameExtent = geometry.source.width == geometry.dest.width &&
                            geometry.source.height == geometry.dest.height;
    if (coversDest && coversSource && sameExtent)
        return PresentKind::BlitPresent;
    return PresentKind::DirectScanout;
}

inline CompositeTarget SelectCompositeTarget(PresentKind kind, bool havePresentSource) {
    switch (kind) {
        case PresentKind::FlipChain:
        case PresentKind::BlitPresent:
            // Without the image that is about to be published there is nothing
            // to composite into, and writing the visible surface instead is the
            // race this policy exists to remove.
            return havePresentSource ? CompositeTarget::PresentSource : CompositeTarget::None;
        case PresentKind::DirectScanout:
            return CompositeTarget::VisibleSurface;
        case PresentKind::None:
            break;
    }
    return CompositeTarget::None;
}

// The rectangle CE has to write this time: at least the overlay's own, and at
// least everything CE wrote into this surface last time. Without the second
// part a shrinking overlay leaves the strip it vacated holding the previous
// composite, with nothing left to ever repaint it.
inline Rect ExpandToPreviousComposite(const Rect& region, bool havePrevious, const Rect& previous) {
    if (!havePrevious || previous.IsEmpty())
        return region;
    Rect merged;
    merged.left = region.left < previous.left ? region.left : previous.left;
    merged.top = region.top < previous.top ? region.top : previous.top;
    merged.right = region.right > previous.right ? region.right : previous.right;
    merged.bottom = region.bottom > previous.bottom ? region.bottom : previous.bottom;
    return merged;
}

// How many writes into a flip chain's front buffer have to pile up without a
// Flip before those writes are the presentation. The first may be incidental
// front-buffer drawing a live flip immediately replaces; the second proves the
// chain has stopped advancing (the common loading-screen shape).
inline constexpr uint32_t kScanoutWritesWithoutFlipThreshold = 2;

inline bool ScanoutWriteIsPresentation(bool destOwnsFlipChain, uint32_t scanoutWritesSinceFlip,
                                       uint32_t threshold = kScanoutWritesWithoutFlipThreshold) {
    return !destOwnsFlipChain || scanoutWritesSinceFlip >= threshold;
}

// A direct-scanout update only needs the overlay restored when it actually
// overwrote the overlay's own pixels.
inline bool DirectScanoutNeedsComposite(const Rect& overlayBounds, bool haveChangedRect, const Rect& changedRect) {
    if (overlayBounds.IsEmpty())
        return false;
    if (!haveChangedRect)
        return true;
    return RectsIntersect(changedRect, overlayBounds);
}

// The composite moves the overlay's own pixels across the CPU, never the whole
// frame: a 4K round trip of the full surface costs about 66 MB per present and
// is what held the DirectDraw route to roughly 17 presents per second.
//
// The rectangle is grown to an alignment so a value row that changes width by a
// few pixels does not resize the raster/backdrop caches, and is clamped to the
// surface so a stale viewport can never produce an out-of-bounds lock.
inline bool AlignCompositeRegion(const Rect& bounds, uint32_t surfaceWidth, uint32_t surfaceHeight, int alignment,
                                 Rect& out) {
    out = Rect{};
    if (surfaceWidth == 0 || surfaceHeight == 0 || bounds.IsEmpty())
        return false;
    if (alignment < 1)
        alignment = 1;

    Rect aligned;
    aligned.left = (bounds.left / alignment) * alignment;
    aligned.top = (bounds.top / alignment) * alignment;
    aligned.right = ((bounds.right + alignment - 1) / alignment) * alignment;
    aligned.bottom = ((bounds.bottom + alignment - 1) / alignment) * alignment;

    if (aligned.left < 0)
        aligned.left = 0;
    if (aligned.top < 0)
        aligned.top = 0;
    if (aligned.right > static_cast<int>(surfaceWidth))
        aligned.right = static_cast<int>(surfaceWidth);
    if (aligned.bottom > static_cast<int>(surfaceHeight))
        aligned.bottom = static_cast<int>(surfaceHeight);
    if (aligned.IsEmpty())
        return false;

    out = aligned;
    return true;
}

// Native pixels are already in the render target when capture reads it. An
// overlay-excluded recording therefore stays on the CPU path, which can capture
// first and composite afterwards.
inline bool NativeOverlayShouldDraw(bool enabled, bool showOverlay, bool recording, bool captureIncludesOverlay) {
    return enabled && showOverlay && (!recording || captureIncludesOverlay);
}

// One premultiplied source-over step: the sprite already carries colour
// multiplied by its own coverage, so the destination only has to be attenuated
// by what the sprite does not cover.
//
// This is what replaces the per-presentation GPU round trip. The composite used
// to read the surface, upload it, blend on the GPU and read the result back -
// and `GetRenderTargetData` blocks the render thread until the GPU has caught
// up, every presentation, on a surface the display is scanning out.
inline uint32_t BlendPremultipliedOver(uint32_t sprite, uint32_t destination) {
    const uint32_t alpha = (sprite >> 24) & 0xFFu;
    if (alpha == 0xFFu)
        return sprite | 0xFF000000u;
    if (alpha == 0u)
        return destination;
    const uint32_t inverse = 255u - alpha;
    const uint32_t blue = ((sprite & 0x000000FFu) + (((destination & 0x000000FFu) * inverse + 127u) / 255u));
    const uint32_t green =
        (((sprite >> 8) & 0xFFu) + ((((destination >> 8) & 0xFFu) * inverse + 127u) / 255u));
    const uint32_t red = (((sprite >> 16) & 0xFFu) + ((((destination >> 16) & 0xFFu) * inverse + 127u) / 255u));
    const uint32_t clampedBlue = blue > 255u ? 255u : blue;
    const uint32_t clampedGreen = green > 255u ? 255u : green;
    const uint32_t clampedRed = red > 255u ? 255u : red;
    return 0xFF000000u | (clampedRed << 16) | (clampedGreen << 8) | clampedBlue;
}

// DirectDraw's 16-bit surfaces store fewer bits than the canonical backdrop.
// State comparisons must retain the expanded value of the bytes actually
// written, not the unquantized blend result, or CE can never recognize its own
// previous composite and repeatedly blends the overlay over itself.
inline uint16_t PackRgb565(uint32_t color) {
    return static_cast<uint16_t>(((color >> 8) & 0xF800u) | ((color >> 5) & 0x07E0u) |
                                 ((color >> 3) & 0x001Fu));
}

inline uint32_t ExpandRgb565(uint16_t value) {
    const uint32_t red = (value >> 11) & 0x1Fu;
    const uint32_t green = (value >> 5) & 0x3Fu;
    const uint32_t blue = value & 0x1Fu;
    return 0xFF000000u | (((red << 3) | (red >> 2)) << 16) |
           (((green << 2) | (green >> 4)) << 8) | ((blue << 3) | (blue >> 2));
}

inline uint16_t PackRgb555(uint32_t color) {
    return static_cast<uint16_t>(((color >> 9) & 0x7C00u) | ((color >> 6) & 0x03E0u) |
                                 ((color >> 3) & 0x001Fu));
}

inline uint32_t ExpandRgb555(uint16_t value) {
    const uint32_t red = (value >> 10) & 0x1Fu;
    const uint32_t green = (value >> 5) & 0x1Fu;
    const uint32_t blue = value & 0x1Fu;
    return 0xFF000000u | (((red << 3) | (red >> 2)) << 16) |
           (((green << 3) | (green >> 2)) << 8) | ((blue << 3) | (blue >> 2));
}

}  // namespace ce::ddraw_present_policy
