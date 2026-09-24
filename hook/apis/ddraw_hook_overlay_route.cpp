#include "ddraw_hook_internal.h"

#include "ddraw_hook_present_overrides.h"
#include "../common/ddraw_chain_lifetime_policy.h"

#include <array>
#include <atomic>

// The native Direct3D 7 renderer is an auxiliary backend. The adapter itself
// stays on the headless CPU backend so a loading screen that switches from
// EndScene/Flip to 2D blits never tears down a device backend or loses dozens of
// frames waiting for a route-stability counter. Native draws happen immediately
// before the application's real EndScene; Flip only observes that the pixels
// are already present. This avoids the synthetic BeginScene/EndScene pair that
// twice drove Steam's co-resident overlay through an invalid frame boundary.

namespace {

namespace policy = ce::ddraw_present_policy;

constexpr size_t kMaxNativeSurfaceStates = 8;

struct NativeSurfaceState {
    uintptr_t identity = 0;
    ce::ddraw_native_overlay::DamageTracker damage;
    uint32_t lastUse = 0;
};

struct NativeOverlayState {
    std::mutex mutex;
    std::unique_ptr<CustomOverlay::D3D7Backend> backend;
    std::array<NativeSurfaceState, kMaxNativeSurfaceStates> surfaces = {};
    uint32_t useCounter = 0;
    void* failureDevice = nullptr;
    uint32_t retryInterval = 0;
    uint32_t retryCountdown = 0;
};

NativeOverlayState g_nativeOverlay;

NativeSurfaceState* FindNativeSurfaceLocked(uintptr_t identity) {
    for (auto& candidate : g_nativeOverlay.surfaces) {
        if (candidate.identity == identity)
            return &candidate;
    }
    return nullptr;
}

NativeSurfaceState* AcquireNativeSurfaceLocked(uintptr_t identity) {
    if (auto* existing = FindNativeSurfaceLocked(identity)) {
        existing->lastUse = ++g_nativeOverlay.useCounter;
        return existing;
    }
    NativeSurfaceState* entry = nullptr;
    for (auto& candidate : g_nativeOverlay.surfaces) {
        if (candidate.identity == 0) {
            entry = &candidate;
            break;
        }
        if (!entry || candidate.lastUse < entry->lastUse)
            entry = &candidate;
    }
    if (!entry)
        return nullptr;
    *entry = {};
    entry->identity = identity;
    entry->lastUse = ++g_nativeOverlay.useCounter;
    return entry;
}

// The sidecar leaves stage 0 unbound and relies on the state block to put the
// application's texture back. Restoring it explicitly removes that dependency:
// a Direct3D 7 title that caches its own bindings and skips a redundant
// SetTexture would otherwise draw its next material untextured if the block
// ever stopped carrying textures. The binding comes from the shadow, which owns
// a reference to it, so this can never hand the runtime a released surface.
void RestoreApplicationStageZeroTexture(IDirect3DDevice7* device) {
    if (!device)
        return;
    IUnknown* texture = AcquireLegacyD3D7TextureBinding(device, 0);
    if (!texture)
        return;
    using SetTexture7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, DWORD, IUnknown*);
    void** deviceVTable = *(void***)device;
    auto setTexture = reinterpret_cast<SetTexture7_t>(deviceVTable[D3D7_VTABLE_SETTEXTURE]);
    if (setTexture) {
        LegacyD3DInternalScope internalScope;
        setTexture(device, 0, texture);
    }
    texture->Release();
}

void ClearNativeSurfacePixelsLocked() {
    for (auto& surface : g_nativeOverlay.surfaces)
        surface.damage.Clear();
}

void RecordNativeBackendFailureLocked(void* device) {
    constexpr uint32_t kMaximumRetryInterval = 512;
    if (g_nativeOverlay.failureDevice != device) {
        g_nativeOverlay.failureDevice = device;
        g_nativeOverlay.retryInterval = 1;
    } else {
        g_nativeOverlay.retryInterval =
            (std::min)(kMaximumRetryInterval, (std::max)(1u, g_nativeOverlay.retryInterval * 2u));
    }
    g_nativeOverlay.retryCountdown = g_nativeOverlay.retryInterval;
}

void ClearNativeBackendFailureLocked() {
    g_nativeOverlay.failureDevice = nullptr;
    g_nativeOverlay.retryInterval = 0;
    g_nativeOverlay.retryCountdown = 0;
}

bool DeferNativeBackendRetryLocked(void* device) {
    if (g_nativeOverlay.failureDevice != device || g_nativeOverlay.retryCountdown == 0)
        return false;
    --g_nativeOverlay.retryCountdown;
    return true;
}

}  // namespace

IDirect3DDevice7* AcquireNativeLegacyD3DDeviceForSurface(IDirectDrawSurface7* presentedSurface) {
    if (!presentedSurface || !GetActiveGraphicsConfig().legacyD3DNativeOverlay)
        return nullptr;

    IDirect3DDevice7* device = AcquireLegacyD3D7Device();
    if (!device)
        return nullptr;

    using GetRenderTarget7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, IDirectDrawSurface7**);
    void** deviceVTable = *(void***)device;
    auto getRenderTarget = reinterpret_cast<GetRenderTarget7_t>(deviceVTable[D3D7_VTABLE_GETRENDERTARGET]);

    IDirectDrawSurface7* renderTarget = nullptr;
    bool targetsThisSurface = false;
    if (getRenderTarget && SUCCEEDED(getRenderTarget(device, &renderTarget)) && renderTarget) {
        targetsThisSurface = renderTarget == presentedSurface ||
                             DirectDrawObjectIdentity(renderTarget) == DirectDrawObjectIdentity(presentedSurface);
    }
    if (renderTarget)
        renderTarget->Release();
    if (!targetsThisSurface) {
        device->Release();
        return nullptr;
    }
    return device;
}

bool EnsureOverlayRouteBackend(DDrawOverlayRoute requiredRoute, IDirect3DDevice7*) {
    if (requiredRoute != DDrawOverlayRoute::HelperComposite)
        return false;

    const OverlayBackendType currentBackend = g_OverlayAdapter.GetBackendType();
    if (currentBackend == OverlayBackendType::CpuRaster) {
        ddraw_hook_g_OverlayRoute = requiredRoute;
        return true;
    }

    if (currentBackend != OverlayBackendType::None) {
        LegacyD3DInternalScope internalScope;
        g_OverlayAdapter.Shutdown();
        ++ddraw_hook_g_OverlayRouteSwitches;
    }
    ddraw_hook_g_DDrawCapture.ReleaseCompositeRegionResources();
    if (ddraw_hook_g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
    }
    if (!g_OverlayAdapter.InitCpuRaster()) {
        HookLogImportant("DDraw: CPU overlay backend unavailable; the overlay stays off for this route");
        return false;
    }
    if (ddraw_hook_g_CachedHwnd)
        g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
    ddraw_hook_g_OverlayRoute = requiredRoute;
    HookLogImportant("DDraw: Overlay adapter -> persistent CPU renderer (switch=%u)",
                     ddraw_hook_g_OverlayRouteSwitches);
    return true;
}

DirectDrawChainReferenceRelease ResetDirectDrawPresentationStateForPrimaryChange() {
    // Surface identities are raw COM identities by design; retaining a
    // reference would keep an obsolete fullscreen chain alive. Drop every
    // byte-derived proof at a primary-chain boundary so allocator address reuse
    // can never make a new surface look like an old composite.
    DirectDrawChainReferenceRelease released;
    ddraw_hook_g_DDrawCapture.ReleaseOverlayResources();
    released.nativeSidecar = ReleaseNativeLegacyD3DOverlay();
    released.presentationReferences = ResetDirectDrawPresentationOverrides();
    ddraw_hook_g_ScanoutWritesSinceFlip.store(0, std::memory_order_relaxed);
    ddraw_hook_g_OverlayRoute = DDrawOverlayRoute::Undecided;
    return released;
}

void ReleaseDirectDrawChainBeforePrimaryCreation(const char* api) {
    DirectDrawChainReferenceRelease released = ResetDirectDrawPresentationStateForPrimaryChange();
    // Only here, not in every reset: a runtime primary noticed mid-stream does
    // not mean the application let go of its device. The tracked device keeps
    // its render target - the old chain's back buffer - alive on its own.
    released.d3d7Device = ReleaseTrackedLegacyD3D7Device();
    // Raw identities of a chain that may now be destroyed; an address DirectDraw
    // hands out again must not be mistaken for the old primary.
    ddraw_hook_g_PrimarySurface = nullptr;
    ddraw_hook_g_PrimarySurface4 = nullptr;
    if (released.Any()) {
        HookLogImportant(
            "DDraw: Released CE's references into the previous presentation chain before the application's %s "
            "primary creation (presentationRefs=%u nativeSidecar=%d d3d7Device=%d)",
            api ? api : "unknown", released.presentationReferences, released.nativeSidecar ? 1 : 0,
            released.d3d7Device ? 1 : 0);
    } else {
        HookLog("DDraw: %s primary creation - CE held no references into a previous chain", api ? api : "unknown");
    }
}

void LogApplicationPrimaryCreationFailure(const char* api, HRESULT hr, uint32_t ordinal) {
    // Rare by nature and the last thing before an application's own fatal
    // error box, so every one is worth a line; the bound only stops a title
    // that retries forever from flooding the log.
    static std::atomic<uint32_t> failures{0};
    const uint32_t failure = failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failure > 16 && (failure & (failure - 1)) != 0)
        return;
    HookLogImportant("DDraw: Application %s primary creation FAILED hr=0x%08X (%s) ordinal=%u failure=%u",
                     api ? api : "unknown", static_cast<unsigned>(hr),
                     ce::ddraw_chain_lifetime::DescribePrimaryCreationFailure(static_cast<uint32_t>(hr)), ordinal,
                     failure);
}

bool PrimeNativeLegacyD3DOverlay(IDirect3DDevice7* device) {
    if (!device || !GetActiveGraphicsConfig().legacyD3DNativeOverlay)
        return false;

    // The sidecar's `D3DSBT_ALL` block restores the application's textures, and
    // a Direct3D 7 block holds them without a reference. Drawing is only legal
    // while CE owns a reference to every binding that restore could put back;
    // otherwise the CPU composite - which touches no device state at all - is
    // the route, and the overlay still appears.
    if (!LegacyD3D7TextureBindingsAreRestoreSafe(device)) {
        static std::atomic<void*> loggedDevice{nullptr};
        if (loggedDevice.exchange(device, std::memory_order_relaxed) != static_cast<void*>(device)) {
            HookLogImportant(
                "DDraw: Native D3D7 sidecar withheld for device=%p - CE does not own its texture bindings, so a "
                "state-block restore could re-bind a released surface; the CPU composite keeps the overlay",
                device);
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    if (g_nativeOverlay.backend && g_nativeOverlay.backend->GetDevice() == static_cast<void*>(device) &&
        g_nativeOverlay.backend->IsUsable()) {
        return true;
    }

    {
        LegacyD3DInternalScope internalScope;
        g_nativeOverlay.backend.reset();
    }
    ClearNativeSurfacePixelsLocked();
    if (DeferNativeBackendRetryLocked(device))
        return false;

    auto backend = std::make_unique<CustomOverlay::D3D7Backend>(device);
    bool initialized = false;
    {
        LegacyD3DInternalScope internalScope;
        initialized = g_OverlayAdapter.InitializeAuxiliaryBackend(*backend);
    }
    if (!initialized) {
        RecordNativeBackendFailureLocked(device);
        HookLogImportant("DDraw: Native D3D7 sidecar initialization failed; retaining the CPU composite");
        return false;
    }
    g_nativeOverlay.backend = std::move(backend);
    // The gate above already refused any device whose texture bindings CE does
    // not own, so saying so here makes a later session provable from the log.
    HookLogImportant(
        "DDraw: Native D3D7 sidecar primed at a presentation boundary (device=%p, texture bindings owned by CE)",
        device);
    return true;
}

bool DrawNativeLegacyD3DOverlayAtEndScene(void* opaqueDevice) {
    auto* device = static_cast<IDirect3DDevice7*>(opaqueDevice);
    SharedMemoryLayout* shared = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool recording = g_IPC && g_IPC->IsRecording();
    const bool screenshotPending = GetPendingScreenshotRequestId(shared) != 0;
    const bool nativeAllowed =
        shared && policy::NativeOverlayShouldDraw(GetActiveGraphicsConfig().legacyD3DNativeOverlay,
                                                  shared->overlayConfig.showOverlay, recording,
                                                  shared->overlayConfig.captureIncludeOverlay, screenshotPending,
                                                  shared->overlayConfig.screenshotIncludeOverlay);
    if (!device || !nativeAllowed) {
        std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
        ClearNativeSurfacePixelsLocked();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
        if (!g_nativeOverlay.backend || g_nativeOverlay.backend->GetDevice() != opaqueDevice) {
            ClearNativeSurfacePixelsLocked();
            return false;
        }
    }

    using GetRenderTarget7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, IDirectDrawSurface7**);
    void** deviceVTable = *(void***)device;
    auto getRenderTarget = reinterpret_cast<GetRenderTarget7_t>(deviceVTable[D3D7_VTABLE_GETRENDERTARGET]);
    IDirectDrawSurface7* renderTarget = nullptr;
    if (!getRenderTarget || FAILED(getRenderTarget(device, &renderTarget)) || !renderTarget)
        return false;

    uint32_t width = 0;
    uint32_t height = 0;
    const uintptr_t identity = DirectDrawObjectIdentity(renderTarget);
    const bool haveGeometry = GetSurfaceSize(renderTarget, width, height) && width > 0 && height > 0;
    if (!identity || !haveGeometry ||
        !ddraw_hook_g_DDrawCapture.EnsureOverlayDevice(ResolveDirectDrawTargetWindow(), width, height) ||
        !PrepareDirectDrawOverlayAdapter(static_cast<int>(width), static_cast<int>(height))) {
        renderTarget->Release();
        return false;
    }

    policy::Rect bounds = {};
    bool rendered = false;
    bool attempted = false;
    {
        std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
        auto* backend = g_nativeOverlay.backend.get();
        NativeSurfaceState* surfaceState = FindNativeSurfaceLocked(identity);
        // Only draw into a surface a real Flip/Blt presentation has already
        // published. D3D7 applications may EndScene offscreen render targets;
        // stamping the UI into one would corrupt a later texture pass.
        if (backend && backend->GetDevice() == opaqueDevice && surfaceState) {
            attempted = true;
            g_OverlayAdapter.RenderOverlay(static_cast<int>(width), static_cast<int>(height));
            RECT renderedBounds = {};
            {
                LegacyD3DInternalScope internalScope;
                rendered = g_OverlayAdapter.RenderWithAuxiliaryBackend(*backend, static_cast<int>(width),
                                                                        static_cast<int>(height), &renderedBounds);
            }
            rendered = rendered && backend->LastRenderSucceeded();
            RestoreApplicationStageZeroTexture(device);
            if (rendered) {
                bounds = {renderedBounds.left, renderedBounds.top, renderedBounds.right, renderedBounds.bottom};
                surfaceState->damage.SetCurrent(bounds);
                ClearNativeBackendFailureLocked();
            } else {
                surfaceState->damage.Clear();
                if (!backend->IsUsable())
                    RecordNativeBackendFailureLocked(device);
                rendered = false;
            }
        }
    }
    renderTarget->Release();

    auto& diagnostics = ddraw_hook_g_PresentationDiagnostics;
    if (rendered) {
        diagnostics.nativeSceneDraws.fetch_add(1, std::memory_order_relaxed);
        ddraw_hook_g_OverlayRoute = DDrawOverlayRoute::NativeLegacyD3D;
    } else if (attempted) {
        diagnostics.nativeDrawFailures.fetch_add(1, std::memory_order_relaxed);
    }
    return rendered;
}

ce::ddraw_native_overlay::State QueryNativeLegacyD3DOverlay(IUnknown* surface, policy::Rect* repairRects,
                                                            size_t repairCapacity, size_t& repairCount) {
    repairCount = 0;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return ce::ddraw_native_overlay::State::Absent;
    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    NativeSurfaceState* surfaceState = FindNativeSurfaceLocked(identity);
    return surfaceState ? surfaceState->damage.CopyRepairs(repairRects, repairCapacity, repairCount)
                        : ce::ddraw_native_overlay::State::Absent;
}

void RecordNativeLegacyD3DSurfaceWrite(IUnknown* surface, bool haveChangedRect,
                                       const policy::Rect& changedRect) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    NativeSurfaceState* state = FindNativeSurfaceLocked(identity);
    if (state)
        state->damage.RecordWrite(haveChangedRect, changedRect);
}

void CompleteNativeLegacyD3DOverlayRepair(IUnknown* surface, const policy::Rect& repairedRect) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    NativeSurfaceState* state = FindNativeSurfaceLocked(identity);
    if (state)
        state->damage.CompleteRepair(repairedRect);
}

void ClearNativeLegacyD3DOverlayState(IUnknown* surface) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    NativeSurfaceState* state = FindNativeSurfaceLocked(identity);
    if (!state)
        return;
    state->damage.Clear();
}

void PublishNativeLegacyD3DOverlay(IUnknown* source, IUnknown* destination, bool flipSwapsSurfaceMemory) {
    const uintptr_t sourceIdentity = DirectDrawObjectIdentity(source);
    const uintptr_t destinationIdentity = DirectDrawObjectIdentity(destination);
    if (!sourceIdentity || !destinationIdentity || sourceIdentity == destinationIdentity)
        return;
    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    AcquireNativeSurfaceLocked(sourceIdentity);
    AcquireNativeSurfaceLocked(destinationIdentity);
    NativeSurfaceState* sourceState = FindNativeSurfaceLocked(sourceIdentity);
    NativeSurfaceState* destinationState = FindNativeSurfaceLocked(destinationIdentity);
    if (!sourceState || !destinationState)
        return;
    if (flipSwapsSurfaceMemory) {
        std::swap(sourceState->damage, destinationState->damage);
    } else {
        destinationState->damage = sourceState->damage;
    }
    sourceState->lastUse = ++g_nativeOverlay.useCounter;
    destinationState->lastUse = ++g_nativeOverlay.useCounter;
}

bool ReleaseNativeLegacyD3DOverlay() {
    std::lock_guard<std::mutex> lock(g_nativeOverlay.mutex);
    LegacyD3DInternalScope internalScope;
    const bool hadBackend = g_nativeOverlay.backend != nullptr;
    g_nativeOverlay.backend.reset();
    g_nativeOverlay.surfaces = {};
    g_nativeOverlay.useCounter = 0;
    ClearNativeBackendFailureLocked();
    return hadBackend;
}
