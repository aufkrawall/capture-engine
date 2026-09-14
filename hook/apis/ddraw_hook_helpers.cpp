#include "ddraw_hook_internal.h"


uintptr_t DirectDrawObjectIdentity(IUnknown* object) {


    if (!object)
        return 0;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity)
        return reinterpret_cast<uintptr_t>(object);
    const uintptr_t value = reinterpret_cast<uintptr_t>(identity);
    identity->Release();
    return value;

}

void AssociateDirectDrawSurface(IUnknown* surface,  ce::graphics_api_identity::DirectDrawVersion version) {


    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ddraw_hook_g_SurfaceDirectDrawVersions[identity] = version;

}

void AssociateLegacyD3DSurface(IUnknown* surface,  unsigned d3dVersion) {


    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ddraw_hook_g_SurfaceLegacyD3DVersions[identity] = d3dVersion;

}

void ActivateDirectDrawSurface(IUnknown* surface,  ce::graphics_api_identity::DirectDrawVersion fallbackVersion) {


    auto version = fallbackVersion;
    unsigned d3dVersion = 0;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto ddIt = ddraw_hook_g_SurfaceDirectDrawVersions.find(identity);
        if (ddIt != ddraw_hook_g_SurfaceDirectDrawVersions.end())
            version = ddIt->second;
        const auto d3dIt = ddraw_hook_g_SurfaceLegacyD3DVersions.find(identity);
        if (d3dIt != ddraw_hook_g_SurfaceLegacyD3DVersions.end())
            d3dVersion = d3dIt->second;
    }
    ddraw_hook_g_ActiveDirectDrawVersion.store(static_cast<int>(version), std::memory_order_release);
    if (d3dVersion == 0)
        d3dVersion = ddraw_hook_g_LegacyD3DCallbackVersion.load(std::memory_order_acquire);
    ddraw_hook_g_ActiveLegacyD3DVersion.store(d3dVersion, std::memory_order_release);

}

UINT QueryD3D7MaxAnisotropy(void* opaqueDevice) {


    if (!opaqueDevice)
        return 1;
    auto* ddraw_hook_device = static_cast<IDirect3DDevice7*>(opaqueDevice);
    void** vtable = *(void***)ddraw_hook_device;
    using GetCaps7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, D3DDEVICEDESC7*);
    auto getCaps = reinterpret_cast<GetCaps7_t>(vtable[3]);
    D3DDEVICEDESC7 caps = {};
    return getCaps && SUCCEEDED(getCaps(ddraw_hook_device, &caps)) ? std::max<DWORD>(1, caps.dwMaxAnisotropy) : 1;

}

UINT QueryD3D6MaxAnisotropy(void* opaqueDevice) {


    if (!opaqueDevice)
        return 1;
    auto* ddraw_hook_device = static_cast<IUnknown*>(opaqueDevice);
    void** vtable = *(void***)ddraw_hook_device;
    using GetCaps6_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, D3DDEVICEDESC*, D3DDEVICEDESC*);
    auto getCaps = reinterpret_cast<GetCaps6_t>(vtable[3]);
    D3DDEVICEDESC halCaps = {};
    D3DDEVICEDESC helCaps = {};
    halCaps.dwSize = sizeof(halCaps);
    helCaps.dwSize = sizeof(helCaps);
    if (!getCaps || FAILED(getCaps(ddraw_hook_device, &halCaps, &helCaps)))
        return 1;
    return std::max<DWORD>(1, halCaps.dwMaxAnisotropy ? halCaps.dwMaxAnisotropy : helCaps.dwMaxAnisotropy);

}

bool ShouldSuppressDirectDrawHooking() {


    if (!IsDXVKD3D9WrapperLoaded()) {
        return false;
    }

    SharedMemoryLayout* shm = GetHookSharedMemory();
    if (!shm || !shm->runtimeState.IsVulkanLayerOwnedByProcess(GetCurrentProcessId())) {
        return false;
    }

    static std::atomic<int> s_suppressionLogCount{0};
    if (s_suppressionLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
        HookLogImportant("DDraw: DXVK d3d9 + Vulkan layer detected - suppressing DirectDraw bootstrap/hooks");
    }
    return true;

}

bool HasHookedVTable(const std::vector<void**>& hookedVTables,  void** vtable) {


    return std::find(hookedVTables.begin(), hookedVTables.end(), vtable) != hookedVTables.end();

}

bool IsPrimarySurfaceDesc(const DDSURFACEDESC2* surfaceDesc) {


    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);

}

bool IsPrimarySurfaceDesc(const DDSURFACEDESC* surfaceDesc) {


    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);

}

bool SurfaceHasCaps(IDirectDrawSurface* surface,  DWORD capsMask) {


    if (!surface)
        return false;
    DDSCAPS caps = {};
    return SUCCEEDED(surface->GetCaps(&caps)) && (caps.dwCaps & capsMask) != 0;

}

bool SurfaceHasCaps(IDirectDrawSurface7* surface,  DWORD capsMask) {


    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;

}

bool SurfaceHasCaps(IDirectDrawSurface4* surface,  DWORD capsMask) {


    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;

}

IDirectDrawSurface7* QuerySurface7(IUnknown* surfaceLike) {


    if (!surfaceLike)
        return nullptr;

    IDirectDrawSurface7* surface7 = nullptr;
    if (FAILED(surfaceLike->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&surface7)))) {
        return nullptr;
    }

    return surface7;

}

bool ResolveSurfaceGeometry(IDirectDrawSurface7* surface,  ce::ddraw_present_policy::Extent& extent,
                            DWORD& caps) {


    extent = {};
    caps = 0;
    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    if (FAILED(surface->GetSurfaceDesc(&desc)))
        return false;

    extent.width = desc.dwWidth;
    extent.height = desc.dwHeight;
    caps = desc.ddsCaps.dwCaps;
    return extent.width > 0 && extent.height > 0;

}

IDirectDrawSurface7* AcquireFlipPresentSource(IDirectDrawSurface7* primarySurface,
                                              IDirectDrawSurface7* destOverride) {


    // An explicit flip target is authoritative: the application named the
    // surface it wants on screen.
    if (destOverride) {
        destOverride->AddRef();
        return destOverride;
    }
    if (!primarySurface)
        return nullptr;

    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer) {
        return backBuffer;
    }
    return nullptr;

}

LegacyD3DSamplerVTableRecord* ResolveLegacyD3DSamplerVTable(
    ce::legacy_d3d_sampler_state::Api api,  void* ddraw_hook_device) {


    if (!ddraw_hook_device)
        return nullptr;

    void** vtable = *(void***)ddraw_hook_device;
    thread_local void** cachedD3D6VTable = nullptr;
    thread_local void** cachedD3D7VTable = nullptr;
    thread_local LegacyD3DSamplerVTableRecord* cachedD3D6Record = nullptr;
    thread_local LegacyD3DSamplerVTableRecord* cachedD3D7Record = nullptr;
    void**& cachedVTable = api == ce::legacy_d3d_sampler_state::Api::D3D7 ? cachedD3D7VTable : cachedD3D6VTable;
    LegacyD3DSamplerVTableRecord*& cachedRecord =
        api == ce::legacy_d3d_sampler_state::Api::D3D7 ? cachedD3D7Record : cachedD3D6Record;
    if (cachedVTable == vtable)
        return cachedRecord;

    std::lock_guard<std::mutex> lock(ddraw_hook_g_LegacyD3DSamplerVTableMutex);
    for (const auto& record : ddraw_hook_g_LegacyD3DSamplerVTables) {
        if (record->api == api && record->vtable == vtable) {
            cachedVTable = vtable;
            cachedRecord = record.get();
            return cachedRecord;
        }
    }
    return nullptr;

}

HWND ResolveDirectDrawTargetWindow() {


    if (ddraw_hook_g_CachedHwnd && IsWindow(ddraw_hook_g_CachedHwnd)) {
        return ddraw_hook_g_CachedHwnd;
    }

    if (ddraw_hook_g_DDrawBootstrapWindow && IsWindow(ddraw_hook_g_DDrawBootstrapWindow)) {
        return ddraw_hook_g_DDrawBootstrapWindow;
    }

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foregroundWindow && GetWindowThreadProcessId(foregroundWindow, &foregroundPid) != 0 &&
        foregroundPid == GetCurrentProcessId()) {
        return foregroundWindow;
    }

    ce::overlay_compat::AuxiliaryProcessWindowInfo info = {};
    if (ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), nullptr, &info) && info.hwnd) {
        return info.hwnd;
    }

    return NULL;

}

void MaybeTrackPrimarySurface(IDirectDrawSurface7* surface,  const char* ddraw_hook_reason) {


    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype || ddraw_hook_g_PrimarySurface)
        return;

    ddraw_hook_g_PrimarySurface = surface;
    HookLog("DDraw: Tracking runtime primary surface from %s (%p)", ddraw_hook_reason, surface);

}

void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface,  const char* ddraw_hook_reason) {


    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype4 || ddraw_hook_g_PrimarySurface4)
        return;

    ddraw_hook_g_PrimarySurface4 = surface;
    HookLog("DDraw: Tracking runtime primary surface4 from %s (%p)", ddraw_hook_reason, surface);

}

void ApplyPrerenderLimitDDraw(IDirectDrawSurface7* surface,  float limit) {


    if (limit < 0.0f)
        return;

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT surface to finish flip
        // (This should be called AFTER the actual Flip call)
        typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
        void** vtable = *(void***)surface;
        GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];  // GetFlipStatus is index 13

        while (pGetFlipStatus(surface, 1 /* DDGFS_ISFLIPDONE */) == 0x887600FA /* DDERR_WASSTILLDRAWING */) {
            std::this_thread::yield();
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

        if (ddraw_hook_g_PrerenderSurfaces.size() != (size_t)lookback) {
            ddraw_hook_g_PrerenderSurfaces.assign(lookback, nullptr);
            ddraw_hook_g_PrerenderIdx = 0;
        }


        uint32_t waitIdx = ddraw_hook_g_PrerenderIdx % (uint32_t)ddraw_hook_g_PrerenderSurfaces.size();
        if (ddraw_hook_g_PrerenderSurfaces[waitIdx]) {
            IDirectDrawSurface7* waitSurf = ddraw_hook_g_PrerenderSurfaces[waitIdx];
            typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
            void** vtable = *(void***)waitSurf;
            GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];

            while (pGetFlipStatus(waitSurf, 1) == 0x887600FA) {
                std::this_thread::yield();
            }
        }

        ddraw_hook_g_PrerenderSurfaces[waitIdx] = surface;
        ddraw_hook_g_PrerenderIdx++;
    }

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = ddraw_hook_g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }

}

namespace {

// Growing the staged rectangle to a grid keeps a value row that widens by a few
// pixels from recreating the staging surfaces every time it changes.
constexpr int kCompositeRegionAlignment = 64;

ce::ddraw_present_policy::Rect ToPolicyRect(const RECT& rect) {
    return ce::ddraw_present_policy::Rect{static_cast<int>(rect.left), static_cast<int>(rect.top),
                                          static_cast<int>(rect.right), static_cast<int>(rect.bottom)};
}

bool RegionContains(const ce::ddraw_present_policy::Rect& outer, const ce::ddraw_present_policy::Rect& inner) {
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
           outer.bottom >= inner.bottom;
}

ce::ddraw_present_policy::Rect RegionUnion(const ce::ddraw_present_policy::Rect& a,
                                           const ce::ddraw_present_policy::Rect& b) {
    ce::ddraw_present_policy::Rect merged;
    merged.left = std::min(a.left, b.left);
    merged.top = std::min(a.top, b.top);
    merged.right = std::max(a.right, b.right);
    merged.bottom = std::max(a.bottom, b.bottom);
    return merged;
}

// The rectangle the overlay's geometry occupies, grown to the staging grid and
// clamped to the frame. False means the overlay drew nothing.
bool ResolveOverlayCompositeRegion(int viewportWidth,  int viewportHeight,  ce::ddraw_present_policy::Rect& region) {


    RECT bounds = {};
    if (!g_OverlayAdapter.GetLastRenderedBounds(viewportWidth, viewportHeight, bounds))
        return false;
    return ce::ddraw_present_policy::AlignCompositeRegion(ToPolicyRect(bounds), static_cast<uint32_t>(viewportWidth),
                                                          static_cast<uint32_t>(viewportHeight),
                                                          kCompositeRegionAlignment, region);

}

}  // namespace

void DrawDDrawOverlay(IDirectDrawSurface7* compositeTarget) {


    auto& capture = ddraw_hook_g_DDrawCapture;
    if (!capture.d3d9DeviceEx || !compositeTarget)
        return;

    if (capture.targetHwnd && capture.targetHwnd != ddraw_hook_g_CachedHwnd) {
        ddraw_hook_g_CachedHwnd = capture.targetHwnd;
        InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
    }

    if (ddraw_hook_g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        ddraw_hook_g_CachedHwnd = capture.targetHwnd;
        if (ddraw_hook_g_CachedHwnd) {
            InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
            g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
        }
        if (g_OverlayAdapter.InitDX9(capture.d3d9DeviceEx)) {
            if (ddraw_hook_g_CachedHwnd) {
                g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
            }
            HookLog("DDraw: OverlayAdapter initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&ddraw_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(capture.droppedFrames.load(std::memory_order_relaxed));
    const auto directDrawVersion = static_cast<ce::graphics_api_identity::DirectDrawVersion>(
        ddraw_hook_g_ActiveDirectDrawVersion.load(std::memory_order_acquire));
    const unsigned d3dVersion = ddraw_hook_g_ActiveLegacyD3DVersion.load(std::memory_order_acquire);
    g_OverlayAdapter.SetGraphicsAPI(ce::graphics_api_identity::LegacyDirectXLabel(directDrawVersion, d3dVersion),
                                    "active DirectDraw presentation surface");

    if (!g_OverlayAdapter.IsInitialized() || capture.width == 0 || capture.height == 0)
        return;

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const int viewportWidth = static_cast<int>(capture.width);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const int viewportHeight = static_cast<int>(capture.height);

    // The game's pixels have to be under the overlay before it is blended, and
    // the region to stage is only known from geometry that already exists. The
    // previous frame's rectangle is the starting estimate; the frame that grows
    // past it is corrected below rather than clipped.
    ce::ddraw_present_policy::Rect staged = {};
    bool staging = ResolveOverlayCompositeRegion(viewportWidth, viewportHeight, staged) &&
                   capture.EnsureCompositeRegionResources(staged) &&
                   capture.CopySurfaceRegionToOverlayBackbuffer(compositeTarget, staged);

    g_OverlayAdapter.RenderOverlay(viewportWidth, viewportHeight);

    ce::ddraw_present_policy::Rect rendered = {};
    if (!ResolveOverlayCompositeRegion(viewportWidth, viewportHeight, rendered)) {
        // Nothing was drawn this frame, so nothing has to reach the surface.
        return;
    }

    if (!staging || !RegionContains(staged, rendered)) {
        staged = staging ? RegionUnion(staged, rendered) : rendered;
        staging = capture.EnsureCompositeRegionResources(staged) &&
                  capture.CopySurfaceRegionToOverlayBackbuffer(compositeTarget, staged) &&
                  g_OverlayAdapter.ResubmitLastFrame(viewportWidth, viewportHeight);
    }

    if (staging && capture.CopyOverlayBackbufferRegionToSurface(compositeTarget, staged)) {
        return;
    }

    // The overlay could not be placed inside the image the application is about
    // to publish. The helper's own swapchain is the only remaining route; it is
    // occluded for as long as the application holds the display, which is why
    // it is never the normal path.
    static std::atomic<int> s_compositeFallbackLogCount{0};
    if (s_compositeFallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
        HookLogImportant("DDraw: In-frame overlay composite unavailable (target=%p %dx%d); falling back to the "
                         "helper swapchain present",
                         compositeTarget, viewportWidth, viewportHeight);
    }
    capture.PresentOverlay();

}

bool GetSurfaceSize(IDirectDrawSurface7* surface,  uint32_t& w,  uint32_t& ddraw_hook_h) {


    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);

    if (surface && SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
        w = desc.dwWidth;
        ddraw_hook_h = desc.dwHeight;
        return true;
    }
    return false;

}
