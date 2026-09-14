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

// The composite is a read-modify-write: it reads the target's pixels, blends
// the overlay over them and writes the result back. That is only correct while
// the pixels it reads are the application's - if the region still holds a
// previous composite, the overlay is blended over itself, and a translucent
// overlay darkens a little more with every repetition.
//
// A flip publishes a freshly rendered image, so the region read after one is
// always clean. Consecutive writes into the same scanout surface with no flip
// between them are not: the application may have changed only part of the
// surface and left the overlay's own pixels in place. Gothic II does both - it
// flips 86 times a second and writes its front buffer 11 times a second during
// gameplay, and during the intro logos the ratio inverts to about ten writes
// per flip - which is what was left strobing.
//
// So the clean pixels are kept: read once for a given surface and rectangle,
// reused for every repeat composite into the same place, and thrown away as
// soon as a flip publishes a new image.
// `regionMatchesLastComposite` is the decisive one: the region was read back
// and is byte-identical to what CE last wrote there, so the application has not
// drawn into it since and the pixels under the overlay are still the ones saved.
// Anything else - a different surface, a different rectangle, a flip or
// blit-present republishing the image, or content that simply differs - means
// what was just read IS the application's frame and becomes the new backdrop.
//
// Reusing a backdrop without that check freezes the game's pixels under the
// overlay for as long as the reuse lasts, which on a loading screen is the
// whole screen.
inline bool CompositeBackdropIsReusable(bool haveBackdrop, bool sameSurface, bool sameRegion, PresentKind kind,
                                        bool regionMatchesLastComposite) {
    if (!haveBackdrop || !sameSurface || !sameRegion || !regionMatchesLastComposite)
        return false;
    // A flip publishes what the application just rendered and a blit-present
    // overwrites its destination from a source, so both bring pixels that are
    // clean by construction and must be read.
    return kind == PresentKind::DirectScanout;
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

// Which renderer can draw a given presentation.
enum class OverlayRoute {
    // The application's own 3D device, straight into the surface it is about to
    // present: no readback, no second device, nothing leaving the GPU.
    NativeDevice,
    // A private helper device plus a CPU round trip. Works for any surface.
    HelperComposite,
};

// The native route only exists where the application's device is rendering into
// the very surface being published. A flip publishes what the device rendered;
// a blit publishes an offscreen image the device is not rendering to, and a
// direct scanout write is not a device operation at all.
inline OverlayRoute SelectOverlayRoute(PresentKind kind, bool deviceRendersThePresentedSurface) {
    if (kind == PresentKind::FlipChain && deviceRendersThePresentedSurface)
        return OverlayRoute::NativeDevice;
    return OverlayRoute::HelperComposite;
}

// Nothing may be rendered while the loaded backend cannot draw for the running
// route. Gothic II session `20260914_182411` is the reason this is a rule and
// not an assumption: the composite route ran with the native backend still
// loaded, so every frame issued `BeginScene`, state changes, a draw and
// `EndScene` on the application's own Direct3D 7 device from the composite
// path - device work at a point the application never asked for - and then read
// back a helper backbuffer the overlay had never been drawn into.
inline bool BackendCanRenderRoute(OverlayRoute route, bool nativeBackendBoundToThisDevice,
                                  bool compositeBackendReady) {
    return route == OverlayRoute::NativeDevice ? nativeBackendBoundToThisDevice : compositeBackendReady;
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

// The cached sprite describes one rectangle of one build of the overlay. Any
// change to either - and the geometry changes whenever a value or the graph
// moves - means it has to be produced again.
inline bool OverlaySpriteIsCurrent(bool haveSprite, bool sameRegion, uint64_t cachedRevision,
                                   uint64_t currentRevision) {
    return haveSprite && sameRegion && cachedRevision == currentRevision;
}

}  // namespace ce::ddraw_present_policy
