#include "ddraw_hook_composite_state.h"

// Overlay composite for the DirectDraw/DX6/DX7 compatibility route.
//
// There is no GPU path from a D3D9Ex device into a DirectDraw surface, so the
// overlay's pixels are produced by the CPU rasterizer and blended into the
// surface the presentation publishes. What lands in the surface is never
// "sprite over whatever is there": the composite keeps the application's
// backdrop under CE's rectangle and compares the surface bytes against the last
// composite before deciding either way. That is what makes a repeat presentation
// idempotent - the same pixels are written no matter how many times the same
// image is composited - while the frame-time graph keeps moving.
//
// Cost is bounded by the dirty rectangle. When the application has not written
// the region (tracked through the hooked Lock, Blt, BltFast, GetDC and Flip
// paths) and only the graph sprite changed, only the graph's band is locked and
// rewritten; when nothing changed at all, no lock happens.

namespace {

namespace policy = ce::ddraw_present_policy;
using ce::ddraw_present_policy::Rect;
using ce::ddraw_present_policy::PresentKind;

constexpr int kCompositeRegionAlignment = 64;

void AccumulateMicroseconds(std::atomic<uint64_t>& total, std::atomic<uint32_t>& maximum, int64_t microseconds) {
    if (microseconds <= 0)
        return;
    total.fetch_add(static_cast<uint64_t>(microseconds), std::memory_order_relaxed);
    uint32_t previous = maximum.load(std::memory_order_relaxed);
    const auto observed = static_cast<uint32_t>(microseconds);
    while (observed > previous &&
           !maximum.compare_exchange_weak(previous, observed, std::memory_order_relaxed)) {
    }
}

// Canonical storage is 0xAARRGGBB, the layout the rasterizer already produces
// and the native layout of a standard 32-bit DirectDraw surface. Reject an
// unusual channel layout instead of silently writing swapped colours.
bool IsRgb888(const DDPIXELFORMAT& format) {
    return (format.dwFlags & DDPF_RGB) != 0 && format.dwRGBBitCount == 32 &&
           format.dwRBitMask == 0x00FF0000u && format.dwGBitMask == 0x0000FF00u &&
           format.dwBBitMask == 0x000000FFu;
}

bool IsRgb565(const DDPIXELFORMAT& format) {
    return (format.dwFlags & DDPF_RGB) != 0 && format.dwRGBBitCount == 16 &&
           format.dwRBitMask == 0xF800u && format.dwGBitMask == 0x07E0u &&
           format.dwBBitMask == 0x001Fu && format.dwRGBAlphaBitMask == 0;
}

bool IsRgb555(const DDPIXELFORMAT& format) {
    return (format.dwFlags & DDPF_RGB) != 0 &&
           (format.dwRGBBitCount == 15 || format.dwRGBBitCount == 16) &&
           format.dwRBitMask == 0x7C00u && format.dwGBitMask == 0x03E0u &&
           format.dwBBitMask == 0x001Fu && format.dwRGBAlphaBitMask == 0;
}

DDrawCapture::DDrawCompositeState::SurfaceState* FindSurfaceState(
    DDrawCapture::DDrawCompositeState& state, uintptr_t identity) {
    for (auto& candidate : state.surfaces) {
        if (candidate.identity == identity)
            return &candidate;
    }
    return nullptr;
}

DDrawCapture::DDrawCompositeState::SurfaceState* AcquireSurfaceState(
    DDrawCapture::DDrawCompositeState& state, uintptr_t identity) {
    if (auto* existing = FindSurfaceState(state, identity)) {
        existing->lastUse = ++state.useCounter;
        return existing;
    }

    DDrawCapture::DDrawCompositeState::SurfaceState* entry = nullptr;
    if (state.surfaces.size() < DDrawCapture::DDrawCompositeState::kMaxSurfaceStates) {
        state.surfaces.push_back({});
        entry = &state.surfaces.back();
    } else {
        entry = &*std::min_element(state.surfaces.begin(), state.surfaces.end(), [](const auto& a, const auto& b) {
            return a.lastUse < b.lastUse;
        });
        *entry = {};
    }
    entry->identity = identity;
    entry->lastUse = ++state.useCounter;
    return entry;
}

void ResizeSurfaceState(DDrawCapture::DDrawCompositeState::SurfaceState& entry, const Rect& region) {
    const Rect previous = entry.region;
    const bool preserve = entry.valid && !previous.IsEmpty() &&
                          entry.backdrop.size() == static_cast<size_t>(previous.right - previous.left) *
                                                       static_cast<size_t>(previous.bottom - previous.top) &&
                          entry.lastComposite.size() == entry.backdrop.size();
    const int width = region.right - region.left;
    const int height = region.bottom - region.top;
    std::vector<uint32_t> backdrop(static_cast<size_t>(width) * height, 0u);
    std::vector<uint32_t> lastComposite(backdrop.size(), 0u);

    if (preserve) {
        const Rect overlap = policy::IntersectRect(previous, region);
        const int previousWidth = previous.right - previous.left;
        const int copyWidth = overlap.right - overlap.left;
        for (int y = overlap.top; y < overlap.bottom; ++y) {
            const size_t source = static_cast<size_t>(y - previous.top) * previousWidth + overlap.left - previous.left;
            const size_t destination = static_cast<size_t>(y - region.top) * width + overlap.left - region.left;
            std::copy_n(entry.backdrop.data() + source, copyWidth, backdrop.data() + destination);
            std::copy_n(entry.lastComposite.data() + source, copyWidth, lastComposite.data() + destination);
        }
    }

    entry.region = region;
    entry.backdrop = std::move(backdrop);
    entry.lastComposite = std::move(lastComposite);
    entry.valid = preserve;
}

// One lock/write pass over `dirty`, a sub-rectangle of `writeRegion`.
//
// When `entry.valid` is set, every pixel read is compared against the composite
// CE wrote there last time: equal means the application has not changed it since
// and the saved backdrop is authoritative, different means what was read is the
// application's new frame and becomes the backdrop. When the state is not valid
// there is no proof, so everything read is taken as the application's.
bool WriteCompositeRegion(DDrawCapture::DDrawCompositeState::SurfaceState& entry,
                          IDirectDrawSurface7* surface, const Rect& writeRegion, const Rect& dirty,
                          const std::vector<uint32_t>& sprite, bool restoreOnly,
                          bool preserveNativeOverlayState) {
    const uint32_t regionWidth = static_cast<uint32_t>(writeRegion.right - writeRegion.left);
    const uint32_t dirtyWidth = static_cast<uint32_t>(dirty.right - dirty.left);
    const uint32_t dirtyHeight = static_cast<uint32_t>(dirty.bottom - dirty.top);
    if (dirtyWidth == 0 || dirtyHeight == 0)
        return true;

    RECT lockRect = {dirty.left, dirty.top, dirty.right, dirty.bottom};
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);

    const int64_t lockStartUs = PerfLogger::GetQpcUs();
    // DDLOCK_NOSYSLOCK is not an optimization and must never be dropped. Without
    // it a DDLOCK_WAIT lock takes the Win16 lock, and this call runs on the
    // application's render thread inside its present, with a co-resident
    // overlay and a message pump in the same process. Gothic II session
    // 20260916_005504 is that shape: this lock started returning E_FAIL, and
    // the render thread never came back out of the presentation a second later.
    // A lock CE cannot take without the system lock is a frame CE does not
    // composite - the caller already treats failure that way.
    const HRESULT lockHr =
        surface->Lock(&lockRect, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR | DDLOCK_NOSYSLOCK, nullptr);
    const int64_t lockUs = PerfLogger::GetQpcUs() - lockStartUs;
    if (FAILED(lockHr) || !desc.lpSurface) {
        static std::atomic<int> s_lockFailLogCount{0};
        if (s_lockFailLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Overlay composite could not lock the presented surface (hr=0x%08x)", lockHr);
        }
        // The write marks were consumed before the lock; put the region back so
        // the next presentation retries instead of trusting a stale backdrop.
        RequeueDirectDrawSurfaceWrite(surface);
        return false;
    }

    const uint32_t* spriteRow = sprite.empty() ? nullptr : sprite.data();
    uint8_t* base = static_cast<uint8_t*>(desc.lpSurface);
    const uint32_t bits = desc.ddpfPixelFormat.dwRGBBitCount;
    const bool is888 = IsRgb888(desc.ddpfPixelFormat);
    const bool is565 = IsRgb565(desc.ddpfPixelFormat);
    const bool is555 = IsRgb555(desc.ddpfPixelFormat);
    bool wrote = false;

    if (is888) {
        const int32_t pitch = desc.lPitch;
        for (uint32_t y = 0; y < dirtyHeight; ++y) {
            const uint32_t regionY = static_cast<uint32_t>(dirty.top + static_cast<int>(y)) -
                                     static_cast<uint32_t>(writeRegion.top);
            auto* row = reinterpret_cast<uint32_t*>(base + static_cast<ptrdiff_t>(y) * pitch);
            const uint32_t* spritePixels = spriteRow ? spriteRow + static_cast<size_t>(regionY) * regionWidth +
                                                           static_cast<size_t>(dirty.left - writeRegion.left)
                                                     : nullptr;
            for (uint32_t x = 0; x < dirtyWidth; ++x) {
                const uint32_t regionX = static_cast<uint32_t>(dirty.left + static_cast<int>(x)) -
                                         static_cast<uint32_t>(writeRegion.left);
                const size_t index = static_cast<size_t>(regionY) * regionWidth + regionX;
                uint32_t application = row[x] | 0xFF000000u;
                if (entry.valid && application == entry.lastComposite[index])
                    application = entry.backdrop[index];
                entry.backdrop[index] = application;
                const uint32_t result = restoreOnly || !spritePixels ? application
                                                                     : policy::BlendPremultipliedOver(spritePixels[x], application);
                entry.lastComposite[index] = result;
                row[x] = result;
            }
        }
        wrote = true;
    } else if (is565 || is555) {
        const int32_t pitch = desc.lPitch;
        for (uint32_t y = 0; y < dirtyHeight; ++y) {
            const uint32_t regionY = static_cast<uint32_t>(dirty.top + static_cast<int>(y)) -
                                     static_cast<uint32_t>(writeRegion.top);
            auto* row = reinterpret_cast<uint16_t*>(base + static_cast<ptrdiff_t>(y) * pitch);
            const uint32_t* spritePixels = spriteRow ? spriteRow + static_cast<size_t>(regionY) * regionWidth +
                                                           static_cast<size_t>(dirty.left - writeRegion.left)
                                                     : nullptr;
            for (uint32_t x = 0; x < dirtyWidth; ++x) {
                const uint32_t regionX = static_cast<uint32_t>(dirty.left + static_cast<int>(x)) -
                                         static_cast<uint32_t>(writeRegion.left);
                const size_t index = static_cast<size_t>(regionY) * regionWidth + regionX;
                const uint32_t read = is565 ? policy::ExpandRgb565(row[x]) : policy::ExpandRgb555(row[x]);
                uint32_t application = read;
                if (entry.valid && application == entry.lastComposite[index])
                    application = entry.backdrop[index];
                entry.backdrop[index] = application;
                const uint32_t result = restoreOnly || !spritePixels ? application
                                                                     : policy::BlendPremultipliedOver(spritePixels[x], application);
                const uint16_t packed = is565 ? policy::PackRgb565(result) : policy::PackRgb555(result);
                row[x] = packed;
                // Keep exactly what the next lock expands from the surface.
                // Comparing against the higher-precision blend result made
                // every 16-bit frame look application-modified.
                entry.lastComposite[index] =
                    is565 ? policy::ExpandRgb565(packed) : policy::ExpandRgb555(packed);
            }
        }
        wrote = true;
    }

    const HRESULT unlockHr = surface->Unlock(&lockRect);
    if (FAILED(unlockHr)) {
        static std::atomic<int> s_unlockFailLogCount{0};
        if (s_unlockFailLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Overlay composite could not unlock the presented surface (hr=0x%08x)",
                             unlockHr);
        }
        RequeueDirectDrawSurfaceWrite(surface);
        return false;
    }

    if (!wrote) {
        static std::atomic<int> s_formatLogCount{0};
        if (s_formatLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Overlay composite cannot write a %u-bit presented surface", bits);
        }
        RequeueDirectDrawSurfaceWrite(surface);
        return false;
    }

    AccumulateMicroseconds(ddraw_hook_g_PresentationDiagnostics.lockMicrosecondsTotal,
                           ddraw_hook_g_PresentationDiagnostics.lockMicrosecondsMax, lockUs);
    if (!preserveNativeOverlayState)
        ClearNativeLegacyD3DOverlayState(surface);
    return true;
}

}  // namespace

bool DDrawCapture::CompositeOverlaySprite(IDirectDrawSurface7* surface, const Rect& overlayBounds, PresentKind kind,
                                          bool haveChangedRect, const Rect& changedRect,
                                          bool repairExistingNative) {
    std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
    if (!surface || overlayBounds.IsEmpty() || !g_OverlayAdapter.IsInitialized())
        return false;
    if (!compositeState)
        compositeState = new DDrawCompositeState();
    auto& state = *compositeState;

    RegisterDirectDrawCompositeSurface(surface);
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return false;

    // Grow the tracked rectangle to the previous one so a shrinking overlay
    // repaints the strip it vacated, then align it on the same 64-pixel grid the
    // composite cache always used.
    Rect alignedOverlay = {};
    if (!policy::AlignCompositeRegion(overlayBounds, width, height, kCompositeRegionAlignment,
                                      alignedOverlay)) {
        return false;
    }
    bool havePreviousRegion = false;
    Rect previousRegion = {};
    DDrawCompositeState::SurfaceState* entry = AcquireSurfaceState(state, identity);
    if (!entry)
        return false;
    havePreviousRegion = !entry->region.IsEmpty();
    previousRegion = entry->region;

    const Rect writeRegion =
        havePreviousRegion ? policy::ExpandToPreviousComposite(alignedOverlay, true, previousRegion) : alignedOverlay;
    const uint32_t regionWidth = static_cast<uint32_t>(writeRegion.right - writeRegion.left);
    const uint32_t regionHeight = static_cast<uint32_t>(writeRegion.bottom - writeRegion.top);
    const size_t pixelCount = static_cast<size_t>(regionWidth) * regionHeight;
    if (regionWidth == 0 || regionHeight == 0)
        return false;

    const bool regionChanged = !havePreviousRegion || previousRegion.left != writeRegion.left ||
                               previousRegion.top != writeRegion.top || previousRegion.right != writeRegion.right ||
                               previousRegion.bottom != writeRegion.bottom;
    if (regionChanged) {
        // Preserve the proof for the old sub-rectangle. Clearing it while the
        // old composite is still in the surface makes CE mistake its own pixels
        // for application output and blend the overlay over itself.
        ResizeSurfaceState(*entry, writeRegion);
        // A target change forces the next cache update through a full rebuild.
        state.spriteCache.target = {};
        state.spriteCache.hasFrame = false;
    }
    if (entry->backdrop.size() != pixelCount)
        entry->backdrop.assign(pixelCount, 0u);
    if (entry->lastComposite.size() != pixelCount)
        entry->lastComposite.assign(pixelCount, 0u);

    ce::overlay_cpu_raster::Target target;
    target.left = writeRegion.left;
    target.top = writeRegion.top;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    target.width = static_cast<int>(regionWidth);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    target.height = static_cast<int>(regionHeight);

    ce::overlay_cpu_raster::RasterStats rasterStats;
    ce::overlay_cpu_raster::PixelRect changedSprite;
    const int64_t rasterStartUs = PerfLogger::GetQpcUs();
    const bool haveSprite = g_OverlayAdapter.RenderRasterCache(state.spriteCache, target, rasterStats, changedSprite);
    const int64_t rasterUs = PerfLogger::GetQpcUs() - rasterStartUs;
    if (!haveSprite) {
        ddraw_hook_g_PresentationDiagnostics.compositeNoGeometry.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto& diagnostics = ddraw_hook_g_PresentationDiagnostics;
    AccumulateMicroseconds(diagnostics.rasterMicrosecondsTotal, diagnostics.rasterMicrosecondsMax, rasterUs);
    diagnostics.rasterPasses.fetch_add(1, std::memory_order_relaxed);
    diagnostics.spriteRasterizations.fetch_add(rasterStats.fullRasters + rasterStats.dirtyRasters,
                                                std::memory_order_relaxed);
    diagnostics.spriteFullRasters.fetch_add(rasterStats.fullRasters, std::memory_order_relaxed);
    if (rasterStats.dirtyRasters)
        diagnostics.spriteIncrementalUpdates.fetch_add(1, std::memory_order_relaxed);
    diagnostics.spriteReuses.fetch_add(rasterStats.spriteReuses, std::memory_order_relaxed);

    Rect spriteDirty = {};
    if (!changedSprite.IsEmpty()) {
        spriteDirty.left = writeRegion.left + changedSprite.left;
        spriteDirty.top = writeRegion.top + changedSprite.top;
        spriteDirty.right = writeRegion.left + changedSprite.right;
        spriteDirty.bottom = writeRegion.top + changedSprite.bottom;
    }

    // A flip or a blit publishes an image the application has just produced, so
    // the whole region has to be read. A direct-scanout write uses the precise
    // mark the matching hook recorded.
    bool applicationWholeRegion =
        !repairExistingNative && (kind != PresentKind::DirectScanout || !entry->valid);
    Rect applicationWrites = {};
    if (repairExistingNative) {
        // The application write replaced these pixels of an otherwise-current
        // native overlay. Repaint exactly those pixels; rasterizing or blending
        // outside them would apply the translucent overlay a second time.
        if (haveChangedRect)
            applicationWrites = policy::IntersectRect(changedRect, writeRegion);
    } else if (!applicationWholeRegion) {
        if (haveChangedRect)
            applicationWrites = policy::IntersectRect(changedRect, writeRegion);
        policy::Rect consumed = {};
        switch (ConsumeDirectDrawSurfaceWrites(surface, consumed)) {
            case DirectDrawWriteRegion::Whole:
                applicationWholeRegion = true;
                break;
            case DirectDrawWriteRegion::Partial:
                applicationWrites =
                    policy::UnionRect(applicationWrites, policy::IntersectRect(consumed, writeRegion));
                break;
            case DirectDrawWriteRegion::None:
                break;
        }
    }

    Rect dirty = repairExistingNative ? applicationWrites : spriteDirty;
    if (!repairExistingNative && applicationWholeRegion) {
        dirty = writeRegion;
        ClearDirectDrawSurfaceWrites(surface);
    } else if (!repairExistingNative) {
        dirty = policy::UnionRect(dirty, applicationWrites);
    }
    dirty = policy::IntersectRect(dirty, writeRegion);
    if (dirty.IsEmpty()) {
        diagnostics.compositesSkippedClean.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const bool fullRegionWrite = dirty.left == writeRegion.left && dirty.top == writeRegion.top &&
                                 dirty.right == writeRegion.right && dirty.bottom == writeRegion.bottom;
    if (fullRegionWrite)
        diagnostics.compositeFullWrites.fetch_add(1, std::memory_order_relaxed);
    else
        diagnostics.compositePartialWrites.fetch_add(1, std::memory_order_relaxed);

    static uint32_t compositeCount = 0;
    ++compositeCount;
    if (regionChanged) {
        HookLogImportant("DDraw: Overlay composite region resized to %ux%u at (%d,%d)", regionWidth, regionHeight,
                         writeRegion.left, writeRegion.top);
    }

    const int64_t writeStartUs = PerfLogger::GetQpcUs();
    const bool wrote = WriteCompositeRegion(*entry, surface, writeRegion, dirty, state.spriteCache.composed, false,
                                            repairExistingNative);
    const int64_t writeUs = PerfLogger::GetQpcUs() - writeStartUs;
    if (!wrote) {
        diagnostics.compositeWriteFailed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    AccumulateMicroseconds(diagnostics.writeMicrosecondsTotal, diagnostics.writeMicrosecondsMax, writeUs);
    diagnostics.surfaceWritePasses.fetch_add(1, std::memory_order_relaxed);
    if (!repairExistingNative)
        entry->valid = true;
    diagnostics.compositeSucceeded.fetch_add(1, std::memory_order_relaxed);
    if (compositeCount <= 4 || (compositeCount % 600) == 0) {
        HookLogImportant("DDraw: Overlay composited into the presented surface (surface=%p region=%ux%u at %d,%d "
                         "dirty=%dx%d frame=%ux%u count=%u)",
                         surface, regionWidth, regionHeight, writeRegion.left, writeRegion.top,
                         dirty.right - dirty.left, dirty.bottom - dirty.top, width, height, compositeCount);
    }
    return true;
}

bool DDrawCapture::RestoreCompositeRegion(IDirectDrawSurface7* surface) {
    std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
    if (!surface || !compositeState)
        return false;
    DDrawCompositeState::SurfaceState* entry =
        FindSurfaceState(*compositeState, DirectDrawObjectIdentity(surface));
    if (!entry || !entry->valid || entry->region.IsEmpty())
        return false;

    // Remove the overlay everywhere CE wrote it. Where the application has
    // drawn since, its pixels win; where it has not, the saved backdrop does.
    const Rect& region = entry->region;
    ddraw_hook_g_PresentationDiagnostics.compositeFullWrites.fetch_add(1, std::memory_order_relaxed);
    const int64_t writeStartUs = PerfLogger::GetQpcUs();
    const bool wrote =
        WriteCompositeRegion(*entry, surface, region, region, std::vector<uint32_t>(), true, false);
    if (!wrote)
        return false;
    AccumulateMicroseconds(ddraw_hook_g_PresentationDiagnostics.writeMicrosecondsTotal,
                           ddraw_hook_g_PresentationDiagnostics.writeMicrosecondsMax, PerfLogger::GetQpcUs() -
                                                                                      writeStartUs);
    ddraw_hook_g_PresentationDiagnostics.surfaceWritePasses.fetch_add(1, std::memory_order_relaxed);
    HookLogImportant("DDraw: Overlay removed from surface=%p region=%dx%d at (%d,%d)", surface,
                     region.right - region.left, region.bottom - region.top, region.left, region.top);
    entry->valid = false;
    ClearDirectDrawSurfaceWrites(surface);
    return true;
}

void DDrawCapture::PublishCompositeState(IUnknown* source, IUnknown* destination, bool flipSwapsSurfaceMemory) {
    std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
    if (!source || !destination || !compositeState)
        return;
    const uintptr_t sourceIdentity = DirectDrawObjectIdentity(source);
    const uintptr_t destinationIdentity = DirectDrawObjectIdentity(destination);
    if (!sourceIdentity || !destinationIdentity || sourceIdentity == destinationIdentity)
        return;

    auto& state = *compositeState;
    DDrawCompositeState::SurfaceState* sourceState = AcquireSurfaceState(state, sourceIdentity);
    AcquireSurfaceState(state, destinationIdentity);
    // Acquiring the second entry can append to the vector, so resolve both
    // pointers again even though its reserved capacity normally keeps them stable.
    sourceState = FindSurfaceState(state, sourceIdentity);
    DDrawCompositeState::SurfaceState* destinationState = FindSurfaceState(state, destinationIdentity);
    if (!sourceState || !destinationState)
        return;

    if (flipSwapsSurfaceMemory) {
        std::swap(sourceState->region, destinationState->region);
        std::swap(sourceState->backdrop, destinationState->backdrop);
        std::swap(sourceState->lastComposite, destinationState->lastComposite);
        std::swap(sourceState->valid, destinationState->valid);
    } else {
        destinationState->region = sourceState->region;
        destinationState->backdrop = sourceState->backdrop;
        destinationState->lastComposite = sourceState->lastComposite;
        destinationState->valid = sourceState->valid;
    }
    sourceState->lastUse = ++state.useCounter;
    destinationState->lastUse = ++state.useCounter;
    ClearDirectDrawSurfaceWrites(source);
    ClearDirectDrawSurfaceWrites(destination);
}

void DDrawCapture::ReleaseCompositeRegionResources() {
    std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
    if (!compositeState)
        return;
    compositeState->spriteCache = {};
    compositeState->spriteCache.composed.shrink_to_fit();
    compositeState->surfaces.clear();
    compositeState->surfaces.shrink_to_fit();
}
