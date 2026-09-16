#pragma once

// Presentation classification shared by the DirectDraw detour units.
//
// Every DirectDraw generation asks the same two questions of a blit-shaped
// call - is this the application's presentation, and which pixels did it change
// - and answers a nested presentation the same way. Those answers lived in one
// translation unit while all the detours did; the Surface4 generation now has
// its own unit, so they live here instead of being written twice.

#include "ddraw_hook_internal.h"

namespace ce::ddraw_detours {

namespace policy = ce::ddraw_present_policy;

// One presentation classification for a blit-shaped call.
struct BlitPresentation {
    policy::PresentKind kind = policy::PresentKind::None;
    policy::Rect changedRect;
    bool haveChangedRect = false;
};

inline bool BlitCopiesSourceExactly(DWORD flags) {
    // Scheduling/presentation hints do not change pixels. Every other Blt flag
    // can key, alpha-blend, fill, rotate, ROP or otherwise transform the source;
    // in that case compose into the destination after the operation instead of
    // assuming an overlay stamped into the source will survive the copy.
    constexpr DWORD kPassThroughFlags =
        DDBLT_ASYNC | DDBLT_WAIT | DDBLT_DONOTWAIT | DDBLT_PRESENTATION | DDBLT_LAST_PRESENTATION;
    return (flags & ~kPassThroughFlags) == 0;
}

inline bool BltFastCopiesSourceExactly(DWORD flags) {
    constexpr DWORD kPassThroughFlags = DDBLTFAST_WAIT | DDBLTFAST_DONOTWAIT;
    return (flags & ~kPassThroughFlags) == 0;
}

// A nested presentation is answered without ever calling CE's own saved
// original: that pointer is what leads back into the injector that re-entered
// CE. See ddraw_hook_present_reentry.cpp. The macro captures the caller of the
// detour, which is the one fact that separates a co-resident overlay from
// DirectDraw itself from CE re-entering its own hook.
#if !defined(CE_DDRAW_RETURN_ADDRESS)
#if defined(__clang__) || defined(__GNUC__)
#define CE_DDRAW_RETURN_ADDRESS() __builtin_extract_return_addr(__builtin_return_address(0))
#else
#define CE_DDRAW_RETURN_ADDRESS() _ReturnAddress()
#endif
#endif

// The nested presentation still has to happen. Gothic II session
// 20260916_021049 dropped one per frame - on the primary surface, the game's
// real screen flip - and the screen kept showing the menu while the 3D scene
// ran. When the saved original's entry carries another injector's patch, CE
// answers with a bypass trampoline that runs DirectDraw's own implementation
// past that patch; otherwise it drops the presentation exactly as before.
//
// `PresentFn` is the detour's own signature, so the nested call is forwarded
// with the arguments it was made with. The bypass runs at most once per
// outermost presentation, which is what keeps this bounded.
template <typename PresentFn, typename... Args>
inline HRESULT AnswerReenteredPresentation(void* surface, size_t slot, const char* operation, void* savedOriginal,
                                           void* returnAddress, PresentFn, Args... args) {
    void* bypass = AcquireDirectDrawPresentEntryBypass(savedOriginal, operation);
    // The scope has already counted this call, so the outermost presentation
    // reads 1 and the first nested one reads 2.
    const bool runReal = policy::NestedPresentationMayRunRealImplementation(
        ddraw_hook_g_PresentDetourDepth - 1, bypass != nullptr, ddraw_hook_g_PresentBypassUsedOnThread);
    if (!runReal) {
        ddraw_hook_g_PresentationDiagnostics.reentrantPresentationsDropped.fetch_add(1, std::memory_order_relaxed);
        NoteDirectDrawPresentCycle(surface, slot, operation, returnAddress, savedOriginal, "dropped");
        return DD_OK;
    }

    ddraw_hook_g_PresentBypassUsedOnThread = true;
    ddraw_hook_g_PresentationDiagnostics.reentrantPresentationsBypassed.fetch_add(1, std::memory_order_relaxed);
    NoteDirectDrawPresentCycle(surface, slot, operation, returnAddress, savedOriginal, "bypass");
    return reinterpret_cast<PresentFn>(bypass)(args...);
}

inline policy::Rect ToPolicyRect(const RECT& rect) {
    return policy::Rect{static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right),
                        static_cast<int>(rect.bottom)};
}

inline bool ReadSurfaceGeometry(IDirectDrawSurface7* surface, policy::Extent& extent, DWORD& caps) {
    return ResolveSurfaceGeometry(surface, extent, caps);
}

inline bool ReadSurfaceGeometry(IDirectDrawSurface4* surface, policy::Extent& extent, DWORD& caps) {
    extent = {};
    caps = 0;
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    if (!surface || FAILED(surface->GetSurfaceDesc(&desc)))
        return false;
    extent.width = desc.dwWidth;
    extent.height = desc.dwHeight;
    caps = desc.ddsCaps.dwCaps;
    return extent.width > 0 && extent.height > 0;
}

inline bool ReadSurfaceGeometry(IDirectDrawSurface* surface, policy::Extent& extent, DWORD& caps) {
    extent = {};
    caps = 0;
    DDSURFACEDESC desc = {};
    desc.dwSize = sizeof(desc);
    if (!surface || FAILED(surface->GetSurfaceDesc(&desc)))
        return false;
    extent.width = desc.dwWidth;
    extent.height = desc.dwHeight;
    caps = desc.ddsCaps.dwCaps;
    return extent.width > 0 && extent.height > 0;
}

// A blit onto a flip chain's images is ordinary drawing - Flip publishes them -
// so only a single-buffered scanout surface can be presented to this way.
template <typename SurfaceT>
BlitPresentation ClassifyBlitCall(SurfaceT* dest, const RECT* destRect, SurfaceT* source, const RECT* sourceRect,
                                  bool sourceCopyIsExact) {
    BlitPresentation result;
    policy::BlitGeometry geometry;
    DWORD destCaps = 0;
    if (!ReadSurfaceGeometry(dest, geometry.dest, destCaps))
        return result;

    // The destination extent makes a null Blt rectangle exact as well. Keep
    // this even for offscreen/back-buffer writes: a native overlay may already
    // be in that surface even though the write is not itself a presentation.
    result.haveChangedRect = true;
    result.changedRect = destRect
                             ? ToPolicyRect(*destRect)
                             : policy::Rect{0, 0, static_cast<int>(geometry.dest.width),
                                            static_cast<int>(geometry.dest.height)};

    // Only the primary surface is being scanned out. A back buffer carries
    // DDSCAPS_BACKBUFFER without DDSCAPS_PRIMARYSURFACE and is published later
    // by Flip; writing the primary is visible the moment the blit completes,
    // flip chain or not.
    geometry.destIsScanout = (destCaps & DDSCAPS_PRIMARYSURFACE) != 0;
    geometry.destIsBackBuffer = !geometry.destIsScanout && (destCaps & DDSCAPS_BACKBUFFER) != 0;
    geometry.destOwnsFlipChain = (destCaps & DDSCAPS_FLIP) != 0;
    if (geometry.destIsBackBuffer || !geometry.destIsScanout) {
        // Flip owns a back buffer and an offscreen destination is not a
        // presentation at all, so the source never has to be described.
        return result;
    }
    if (destRect) {
        geometry.haveDestRect = true;
        geometry.destRect = ToPolicyRect(*destRect);
    }

    DWORD sourceCaps = 0;
    if (sourceCopyIsExact && source && ReadSurfaceGeometry(source, geometry.source, sourceCaps)) {
        geometry.haveSource = true;
        if (sourceRect) {
            geometry.haveSourceRect = true;
            geometry.sourceRect = ToPolicyRect(*sourceRect);
        }
    }

    result.kind = policy::ClassifyBlit(geometry);
    return result;
}

// BltFast has no destination rectangle: the destination is the source's extent
// placed at the given corner.
template <typename SurfaceT>
BlitPresentation ClassifyBltFastCall(SurfaceT* dest, DWORD destX, DWORD destY, SurfaceT* source,
                                     const RECT* sourceRect, bool sourceCopyIsExact) {
    policy::Extent sourceExtent;
    DWORD sourceCaps = 0;
    const bool haveSource = source && ReadSurfaceGeometry(source, sourceExtent, sourceCaps);
    int copyWidth = haveSource ? static_cast<int>(sourceExtent.width) : 0;
    int copyHeight = haveSource ? static_cast<int>(sourceExtent.height) : 0;
    if (sourceRect) {
        copyWidth = static_cast<int>(sourceRect->right - sourceRect->left);
        copyHeight = static_cast<int>(sourceRect->bottom - sourceRect->top);
    }
    RECT destRect = {static_cast<LONG>(destX), static_cast<LONG>(destY),
                     static_cast<LONG>(destX) + copyWidth, static_cast<LONG>(destY) + copyHeight};
    return ClassifyBlitCall(dest, &destRect, source, sourceRect, sourceCopyIsExact);
}

inline IDirectDrawSurface4* AcquireFlipPresentSource4(IDirectDrawSurface4* primarySurface,
                                               IDirectDrawSurface4* destOverride) {
    if (destOverride) {
        destOverride->AddRef();
        return destOverride;
    }
    if (!primarySurface)
        return nullptr;
    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface4* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer)
        return backBuffer;
    return nullptr;
}

inline IDirectDrawSurface* AcquireFlipPresentSourceLegacy(IDirectDrawSurface* primarySurface,
                                                   IDirectDrawSurface* destOverride) {
    if (destOverride) {
        destOverride->AddRef();
        return destOverride;
    }
    if (!primarySurface)
        return nullptr;
    DDSCAPS backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer)
        return backBuffer;
    return nullptr;
}

}  // namespace ce::ddraw_detours
