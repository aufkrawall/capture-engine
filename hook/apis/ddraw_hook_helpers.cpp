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
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        ddraw_hook_g_SurfaceDirectDrawVersions[identity] = version;
    }
    ddraw_hook_g_SurfaceAssociationGeneration.fetch_add(1, std::memory_order_release);

}

void AssociateLegacyD3DSurface(IUnknown* surface,  unsigned d3dVersion) {


    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        ddraw_hook_g_SurfaceLegacyD3DVersions[identity] = d3dVersion;
    }
    ddraw_hook_g_SurfaceAssociationGeneration.fetch_add(1, std::memory_order_release);

}

void ActivateDirectDrawSurface(IUnknown* surface,  ce::graphics_api_identity::DirectDrawVersion fallbackVersion) {


    // Every hooked Flip, Blt, BltFast and Unlock lands here, so the repeat case
    // - the same surface, frame after frame - must not cost a QueryInterface, a
    // lock and two hash lookups. The memo is per thread and is invalidated by
    // any new association, so it can never answer for a reused address.
    struct ActivationMemo {
        IUnknown* surface = nullptr;
        uint32_t generation = 0;
        int version = 0;
        unsigned d3dVersion = 0;
        bool valid = false;
    };
    thread_local ActivationMemo memo;

    const uint32_t generation = ddraw_hook_g_SurfaceAssociationGeneration.load(std::memory_order_acquire);
    if (memo.valid && memo.surface == surface && memo.generation == generation) {
        ddraw_hook_g_ActiveDirectDrawVersion.store(memo.version, std::memory_order_release);
        unsigned activeD3DVersion = memo.d3dVersion;
        if (activeD3DVersion == 0)
            activeD3DVersion = ddraw_hook_g_LegacyD3DCallbackVersion.load(std::memory_order_acquire);
        ddraw_hook_g_ActiveLegacyD3DVersion.store(activeD3DVersion, std::memory_order_release);
        return;
    }

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
    memo.surface = surface;
    memo.generation = generation;
    memo.version = static_cast<int>(version);
    memo.d3dVersion = d3dVersion;
    memo.valid = true;

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


    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype || surface == ddraw_hook_g_PrimarySurface ||
        !SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        return;
    }

    if (ddraw_hook_g_PrimarySurface)
        ResetDirectDrawPresentationStateForPrimaryChange();
    ddraw_hook_g_PrimarySurface = surface;
    HookLog("DDraw: Tracking runtime primary surface from %s (%p)", ddraw_hook_reason, surface);

}

bool ScanoutSurfaceOwnsFlipChain(IDirectDrawSurface7* surface) {
    ce::ddraw_present_policy::Extent extent = {};
    DWORD caps = 0;
    return ResolveSurfaceGeometry(surface, extent, caps) && (caps & DDSCAPS_FLIP) != 0;
}

void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface,  const char* ddraw_hook_reason) {


    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype4 || surface == ddraw_hook_g_PrimarySurface4 ||
        !SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        return;
    }

    if (ddraw_hook_g_PrimarySurface4)
        ResetDirectDrawPresentationStateForPrimaryChange();
    ddraw_hook_g_PrimarySurface4 = surface;
    HookLog("DDraw: Tracking runtime primary surface4 from %s (%p)", ddraw_hook_reason, surface);

}

namespace {

// Growing the composite rectangle to a grid keeps a value row that widens by a
// few pixels from resizing the raster/backdrop caches every time it changes.
constexpr int kCompositeRegionAlignment = 64;

ce::ddraw_present_policy::Rect ToPolicyRect(const RECT& rect) {
    return ce::ddraw_present_policy::Rect{static_cast<int>(rect.left), static_cast<int>(rect.top),
                                          static_cast<int>(rect.right), static_cast<int>(rect.bottom)};
}

// The rectangle the overlay's geometry occupies, grown to the cache grid and
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

void TrackLegacyD3D7Device(IDirect3DDevice7* device) {


    // The unchanged case is the whole hot path: this runs from
    // SetTextureStageState, which a DX7 title calls for every material it
    // binds. Only an actual device change pays for the lock.
    if (!device || ddraw_hook_g_D3D7Device.load(std::memory_order_relaxed) == device)
        return;

    // CE's reference must never be a device's last one after the application
    // has released the surfaces the device renders to: Direct3D destroys a
    // device through its render target, and Gothic II `20260924_235830`
    // faulted in ~CDirect3DDevice7 exactly that way. The Release interception
    // drops this reference inside the application's own last Release; without
    // it CE does not hold the device at all and the native route stays off.
    if (!LegacyD3D7DeviceReleaseIsIntercepted(device)) {
        static std::atomic<void*> loggedDevice{nullptr};
        if (loggedDevice.exchange(device, std::memory_order_relaxed) != static_cast<void*>(device)) {
            HookLogImportant("DDraw: Not tracking D3D7 device=%p - its Release is not intercepted, so CE could "
                             "outlive the application's reference; the CPU composite keeps the overlay",
                             device);
        }
        return;
    }

    IDirect3DDevice7* previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        previous = ddraw_hook_g_D3D7Device.load(std::memory_order_relaxed);
        if (previous == device)
            return;
        device->AddRef();
        ddraw_hook_g_D3D7Device.store(device, std::memory_order_release);
    }
    // Every reader takes its reference under the lock, and the global no longer
    // names `previous`, so this reference is CE's alone to drop. Outside the
    // lock and through the intercepted Release, so that interception can see
    // whether the application still owns the device.
    if (previous)
        previous->Release();

}

IDirect3DDevice7* AcquireLegacyD3D7Device() {


    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    IDirect3DDevice7* device = ddraw_hook_g_D3D7Device.load(std::memory_order_relaxed);
    if (device)
        device->AddRef();
    return device;

}

bool TrackedLegacyD3D7DeviceIs(void* device) {
    return device && ddraw_hook_g_D3D7Device.load(std::memory_order_acquire) == device;
}

bool ReleaseTrackedLegacyD3D7DeviceIf(void* device) {
    IDirect3DDevice7* tracked = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        if (!device || ddraw_hook_g_D3D7Device.load(std::memory_order_relaxed) != device)
            return false;
        tracked = ddraw_hook_g_D3D7Device.exchange(nullptr, std::memory_order_acq_rel);
    }
    // Outside the lock on purpose: this can be the device's last reference, and
    // Direct3D tears the device down through DirectDraw interfaces CE hooks.
    // Nobody can acquire it any more; earlier acquirers hold their own refs.
    tracked->Release();
    return true;
}

bool PrepareDirectDrawOverlayAdapter(int viewportWidth, int viewportHeight) {
    auto& capture = ddraw_hook_g_DDrawCapture;
    std::lock_guard<std::recursive_mutex> captureLock(capture.captureMutex);
    if (viewportWidth <= 0 || viewportHeight <= 0)
        return false;

    if (capture.targetHwnd && capture.targetHwnd != ddraw_hook_g_CachedHwnd) {
        ddraw_hook_g_CachedHwnd = capture.targetHwnd;
    }
    if (ddraw_hook_g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
    }

    g_OverlayAdapter.SetMetrics(&ddraw_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(capture.droppedFrames.load(std::memory_order_relaxed));
    const auto directDrawVersion = static_cast<ce::graphics_api_identity::DirectDrawVersion>(
        ddraw_hook_g_ActiveDirectDrawVersion.load(std::memory_order_acquire));
    const unsigned d3dVersion = ddraw_hook_g_ActiveLegacyD3DVersion.load(std::memory_order_acquire);
    g_OverlayAdapter.SetGraphicsAPI(ce::graphics_api_identity::LegacyDirectXLabel(directDrawVersion, d3dVersion),
                                    "active DirectDraw presentation surface");
    return EnsureOverlayRouteBackend(DDrawOverlayRoute::HelperComposite, nullptr);
}

void DrawDDrawOverlay(IDirectDrawSurface7* compositeTarget, ce::ddraw_present_policy::PresentKind kind,
                      bool haveChangedRect, const ce::ddraw_present_policy::Rect& changedRect) {
    auto& capture = ddraw_hook_g_DDrawCapture;
    std::lock_guard<std::recursive_mutex> captureLock(capture.captureMutex);
    if (!compositeTarget || capture.width == 0 || capture.height == 0)
        return;

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const int viewportWidth = static_cast<int>(capture.width);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const int viewportHeight = static_cast<int>(capture.height);
    if (!PrepareDirectDrawOverlayAdapter(viewportWidth, viewportHeight))
        return;

    const auto noteNativePresentation = [&]() {
        auto& diagnostics = ddraw_hook_g_PresentationDiagnostics;
        const uint32_t nativeCount = diagnostics.nativePresentations.fetch_add(1, std::memory_order_relaxed) + 1;
        ddraw_hook_g_OverlayRoute = DDrawOverlayRoute::NativeLegacyD3D;
        if (nativeCount <= 4 || (nativeCount % 600) == 0) {
            HookLogImportant(
                "DDraw: Presentation reused the overlay drawn at the application's EndScene "
                "(surface=%p kind=%d %dx%d count=%u)",
                compositeTarget, static_cast<int>(kind), viewportWidth, viewportHeight, nativeCount);
        }
    };
    const auto accumulateCompositeTime = [&](int64_t startedUs) {
        const int64_t compositeUs = PerfLogger::GetQpcUs() - startedUs;
        if (compositeUs <= 0)
            return;
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        diag.compositeMicrosecondsTotal.fetch_add(static_cast<uint64_t>(compositeUs), std::memory_order_relaxed);
        diag.compositeTimedPresentations.fetch_add(1, std::memory_order_relaxed);
        uint32_t previousMax = diag.compositeMicrosecondsMax.load(std::memory_order_relaxed);
        const auto observed = static_cast<uint32_t>(compositeUs);
        while (observed > previousMax &&
               !diag.compositeMicrosecondsMax.compare_exchange_weak(previousMax, observed,
                                                                    std::memory_order_relaxed)) {
        }
    };

    // EndScene may already have drawn the overlay into this exact buffer. A
    // later exact 2D write damages only named pixels; repair those from the
    // unchanged draw list and leave every other native pixel alone. This is the
    // transition that otherwise double-blends the panel when a loading screen
    // takes over from 3D rendering.
    ce::ddraw_present_policy::Rect repairRects[ce::ddraw_native_overlay::DamageTracker::kMaxRepairRects] = {};
    size_t repairCount = 0;
    const ce::ddraw_native_overlay::State nativeState =
        QueryNativeLegacyD3DOverlay(compositeTarget, repairRects,
                                    ce::ddraw_native_overlay::DamageTracker::kMaxRepairRects, repairCount);
    if (nativeState == ce::ddraw_native_overlay::State::Current) {
        noteNativePresentation();
        return;
    }
    if (nativeState == ce::ddraw_native_overlay::State::Repairable) {
        ce::ddraw_present_policy::Rect rendered = {};
        if (!ResolveOverlayCompositeRegion(viewportWidth, viewportHeight, rendered)) {
            ddraw_hook_g_PresentationDiagnostics.compositeNoGeometry.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const int64_t compositeStartUs = PerfLogger::GetQpcUs();
        bool repaired = true;
        for (size_t i = 0; i < repairCount; ++i) {
            if (!capture.CompositeOverlaySprite(compositeTarget, rendered, kind, true, repairRects[i], true)) {
                repaired = false;
                break;
            }
            CompleteNativeLegacyD3DOverlayRepair(compositeTarget, repairRects[i]);
            ddraw_hook_g_PresentationDiagnostics.nativeRepairRegions.fetch_add(1, std::memory_order_relaxed);
        }
        accumulateCompositeTime(compositeStartUs);
        if (repaired) {
            ClearDirectDrawSurfaceWrites(compositeTarget);
            noteNativePresentation();
        }
        return;
    }
    if (nativeState == ce::ddraw_native_overlay::State::Unsafe) {
        const uint32_t occurrence =
            ddraw_hook_g_PresentationDiagnostics.nativeUnsafeDeferrals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (occurrence <= 4 || (occurrence & (occurrence - 1)) == 0) {
            HookLogImportant(
                "DDraw: Deferring CPU overlay after an unbounded write over native D3D7 pixels "
                "(surface=%p kind=%d occurrence=%u)",
                compositeTarget, static_cast<int>(kind), occurrence);
        }
        return;
    }

    // The shared renderer builds the draw list exactly as it does for the GPU
    // backends; the CPU composite rasterizes it and writes it into the surface
    // the presentation publishes. Nothing goes to the GPU and nothing is read
    // back, so the application's own draw and the overlay landing are separated
    // by a memory pass rather than a GPU round trip.
    g_OverlayAdapter.RenderOverlay(viewportWidth, viewportHeight);

    // Initialization creates and uploads the font texture. Doing it here, at a
    // presentation boundary outside BeginScene/EndScene, keeps resource work
    // out of the application's active scene; the following frame can then use
    // the zero-copy EndScene path.
    if (kind != ce::ddraw_present_policy::PresentKind::DirectScanout) {
        IDirect3DDevice7* nativeDevice = AcquireNativeLegacyD3DDeviceForSurface(compositeTarget);
        if (nativeDevice) {
            PrimeNativeLegacyD3DOverlay(nativeDevice);
            nativeDevice->Release();
        }
    }

    ce::ddraw_present_policy::Rect rendered = {};
    if (!ResolveOverlayCompositeRegion(viewportWidth, viewportHeight, rendered)) {
        ddraw_hook_g_PresentationDiagnostics.compositeNoGeometry.fetch_add(1, std::memory_order_relaxed);
        // A valid CPU composite can outlive the draw list when the last
        // visible row/notification disappears. Partial-update applications
        // may never repaint the old rectangle themselves, so remove CE's
        // pixels from the saved backdrop as soon as the renderer becomes
        // empty instead of leaving a stale panel on screen.
        capture.RestoreCompositeRegion(compositeTarget);
        return;
    }

    const int64_t compositeStartUs = PerfLogger::GetQpcUs();
    const bool composited =
        capture.CompositeOverlaySprite(compositeTarget, rendered, kind, haveChangedRect, changedRect);
    accumulateCompositeTime(compositeStartUs);
    // Success and write-failure counters belong to the composite itself, which
    // knows whether it wrote or deliberately skipped a clean region.
    (void)composited;
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
