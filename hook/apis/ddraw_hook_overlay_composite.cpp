#include "ddraw_hook_internal.h"

// Overlay composite for the DirectDraw/DX6/DX7 compatibility route.
//
// The overlay is drawn by the D3D9Ex helper device and then has to end up
// inside a DirectDraw surface, which means a CPU round trip: the surface's
// pixels go up into the helper back buffer, the overlay is blended over them,
// and the result comes back down. Only the overlay's own bounding rectangle
// makes that trip. Moving the whole frame instead cost about 66 MB per present
// at 4K and held Gothic II to roughly 17 presents per second.

namespace {

using ce::ddraw_present_policy::Rect;

uint32_t Unpack565ToBgra(uint16_t value) {
    const uint32_t r = ((value >> 11) & 0x1Fu);
    const uint32_t g = ((value >> 5) & 0x3Fu);
    const uint32_t b = (value & 0x1Fu);
    // Replicate the high bits into the low ones so full-scale inputs stay full scale.
    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 2) | (g >> 4);
    const uint32_t b8 = (b << 3) | (b >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

uint32_t Unpack555ToBgra(uint16_t value) {
    const uint32_t r = ((value >> 10) & 0x1Fu);
    const uint32_t g = ((value >> 5) & 0x1Fu);
    const uint32_t b = (value & 0x1Fu);
    const uint32_t r8 = (r << 3) | (r >> 2);
    const uint32_t g8 = (g << 3) | (g >> 2);
    const uint32_t b8 = (b << 3) | (b >> 2);
    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

uint16_t Pack565FromBgra(uint32_t color) {
    return static_cast<uint16_t>(((color >> 8) & 0xF800u) | ((color >> 5) & 0x07E0u) | ((color >> 3) & 0x001Fu));
}

uint16_t Pack555FromBgra(uint32_t color) {
    return static_cast<uint16_t>(((color >> 9) & 0x7C00u) | ((color >> 6) & 0x03E0u) | ((color >> 3) & 0x001Fu));
}

bool Is565(const DDPIXELFORMAT& format) {
    return format.dwGBitMask == 0x07E0u;
}

}  // namespace

bool DDrawCapture::EnsureCompositeRegionResources(const Rect& region) {
    // Nothing on the GPU is needed for the composite any more; only the sprite
    // buffer has to match the rectangle being written.
    const uint32_t neededWidth = static_cast<uint32_t>(region.right - region.left);
    const uint32_t neededHeight = static_cast<uint32_t>(region.bottom - region.top);
    if (neededWidth == 0 || neededHeight == 0)
        return false;

    if (regionWidth != neededWidth || regionHeight != neededHeight) {
        regionWidth = neededWidth;
        regionHeight = neededHeight;
        HookLogImportant("DDraw: Overlay composite region resized to %ux%u at (%d,%d)", regionWidth, regionHeight,
                         region.left, region.top);
    }
    compositeStateRegion = region;
    return true;
}

void DDrawCapture::InvalidateCompositeBackdrop() {
    compositeStateRegion = {};
}

void DDrawCapture::ReleaseCompositeRegionResources() {
    InvalidateCompositeBackdrop();
    overlaySprite.clear();
    overlaySprite.shrink_to_fit();
    spriteRegion = {};
    spriteRevision = 0;
    regionWidth = 0;
    regionHeight = 0;
}

bool DDrawCapture::BlendOverlaySpriteIntoSurface(IDirectDrawSurface7* surface, const Rect& region) {
    if (!surface)
        return false;

    const uint32_t regionW = static_cast<uint32_t>(region.right - region.left);
    const uint32_t regionH = static_cast<uint32_t>(region.bottom - region.top);
    if (regionW == 0 || regionH == 0)
        return false;

    // The overlay is redrawn on every presentation but its content changes far
    // less often, so the rasterized sprite is reused until the geometry or the
    // rectangle actually moves.
    const uint64_t revision = g_OverlayAdapter.GetLastDrawDataRevision();
    const bool sameRegion = spriteRegion.left == region.left && spriteRegion.top == region.top &&
                            spriteRegion.right == region.right && spriteRegion.bottom == region.bottom;
    if (!ce::ddraw_present_policy::OverlaySpriteIsCurrent(!overlaySprite.empty(), sameRegion, spriteRevision,
                                                          revision)) {
        ce::overlay_cpu_raster::Target target;
        target.left = region.left;
        target.top = region.top;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        target.width = static_cast<int>(regionW);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        target.height = static_cast<int>(regionH);
        if (!g_OverlayAdapter.RasterizeLastFrame(target, overlaySprite)) {
            spriteRevision = 0;
            return false;
        }
        spriteRegion = region;
        spriteRevision = revision;
        ddraw_hook_g_PresentationDiagnostics.spriteRasterizations.fetch_add(1, std::memory_order_relaxed);
    } else {
        ddraw_hook_g_PresentationDiagnostics.spriteReuses.fetch_add(1, std::memory_order_relaxed);
    }
    if (overlaySprite.size() != static_cast<size_t>(regionW) * regionH)
        return false;

    RECT destRect = {region.left, region.top, region.right, region.bottom};
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    const HRESULT lockHr = surface->Lock(&destRect, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
    if (FAILED(lockHr) || !desc.lpSurface) {
        static int lockFailLogCount = 0;
        if (lockFailLogCount < 4) {
            HookLog("DDraw: Overlay blend could not lock the presented surface (hr=0x%08x)", lockHr);
            lockFailLogCount++;
        }
        return false;
    }

    bool blended = false;
    uint8_t* dst = static_cast<uint8_t*>(desc.lpSurface);
    const uint32_t* sprite = overlaySprite.data();
    const uint32_t bits = desc.ddpfPixelFormat.dwRGBBitCount;
    if (bits == 32) {
        for (uint32_t y = 0; y < regionH; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(dst);
            for (uint32_t x = 0; x < regionW; ++x) {
                row[x] = ce::ddraw_present_policy::BlendPremultipliedOver(sprite[x], row[x]);
            }
            sprite += regionW;
            dst += desc.lPitch;
        }
        blended = true;
    } else if (bits == 16) {
        const bool is565 = Is565(desc.ddpfPixelFormat);
        for (uint32_t y = 0; y < regionH; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(dst);
            for (uint32_t x = 0; x < regionW; ++x) {
                const uint32_t existing = is565 ? Unpack565ToBgra(row[x]) : Unpack555ToBgra(row[x]);
                const uint32_t merged = ce::ddraw_present_policy::BlendPremultipliedOver(sprite[x], existing);
                row[x] = is565 ? Pack565FromBgra(merged) : Pack555FromBgra(merged);
            }
            sprite += regionW;
            dst += desc.lPitch;
        }
        blended = true;
    }
    surface->Unlock(&destRect);

    if (!blended) {
        static int formatLogCount = 0;
        if (formatLogCount < 4) {
            HookLog("DDraw: Overlay blend cannot write a %u-bit presented surface", bits);
            formatLogCount++;
        }
        return false;
    }

    static uint32_t compositeCount = 0;
    compositeCount++;
    if (compositeCount <= 4 || (compositeCount % 600 == 0)) {
        HookLogImportant("DDraw: Overlay blended into the presented surface (hwnd=%p region=%ux%u at %d,%d "
                         "frame=%ux%u count=%u)",
                         targetHwnd, regionW, regionH, region.left, region.top, width, height, compositeCount);
    }
    return true;
}

bool DDrawCapture::PresentOverlay() {
    if (!d3d9DeviceEx) {
        return false;
    }

    HWND presentWindowOverride = d3d9UsesFlipEx ? nullptr : targetHwnd;
    HRESULT hr = S_OK;
    {
        DX9InternalBypassScope dx9Bypass;
        hr = d3d9DeviceEx->PresentEx(nullptr, nullptr, presentWindowOverride, nullptr, 0);
    }
    static uint32_t overlayPresentCount = 0;
    overlayPresentCount++;
    if (overlayPresentCount <= 4 || (overlayPresentCount % 600 == 0)) {
        HookLogImportant("DDraw: Overlay helper PresentEx fallback hr=0x%08X hwnd=%p size=%ux%u count=%u",
                         static_cast<unsigned>(hr), targetHwnd, width, height, overlayPresentCount);
    }

    if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING) {
        HookLog("DDraw: Overlay helper present failed (hr=0x%08x)", hr);
    }

    return SUCCEEDED(hr);
}
