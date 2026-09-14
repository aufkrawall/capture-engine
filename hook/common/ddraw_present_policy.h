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
// few pixels does not force the staging surfaces to be recreated, and is
// clamped to the surface so a stale viewport can never produce an out-of-bounds
// lock.
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

}  // namespace ce::ddraw_present_policy
