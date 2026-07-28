#include "ddraw_hook.h"
#include <algorithm>
#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif
#ifndef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0700
#endif
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d9.h>
#include <ddraw.h>
typedef DWORD D3DCOLORMODEL;
typedef float D3DVALUE;
#include <d3dcaps.h>
#include <dxgi.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/frame_timing.h"
#include "../common/freeze_watchdog.h"
#include "../common/graphics_api_identity.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../../common/secure_dll_loading.h"
#include "../common/overlay_compat.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "dx9_hook.h"
#include "hook_common.h"
#include "legacy_d3d_sampler_state.h"
#include "performance_metrics.h"

extern HMODULE g_hModule;

struct IDirect3DDevice7;

struct IDirect3D7 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumDevices(void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**) = 0;
};

struct IDirect3DDevice7 : public IUnknown {};

static const GUID kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
static const GUID kIID_IDirect3D3 = {0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e}};
static const GUID kIID_IDirectDraw3 = {0x618f8ad4, 0x8b7a, 0x11d0, {0x8f, 0xcc, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};
static const GUID kIID_IDirect3DHALDevice = {
    0x84E63dE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};

// D3D7 function typedef
typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState7_t)(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                            DWORD Value);
typedef HRESULT(STDMETHODCALLTYPE* GetTextureStageState7_t)(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                            DWORD* pValue);
typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState6_t)(IUnknown* device, DWORD Stage, DWORD Type, DWORD Value);
typedef HRESULT(STDMETHODCALLTYPE* GetTextureStageState6_t)(IUnknown* device, DWORD Stage, DWORD Type, DWORD* pValue);
typedef HRESULT(STDMETHODCALLTYPE* LegacyD3DEndScene_t)(void* device);
typedef HRESULT(STDMETHODCALLTYPE* D3D7ApplyStateBlock_t)(void* device, DWORD blockHandle);

typedef HRESULT(STDMETHODCALLTYPE* SetRenderState7_t)(IDirect3DDevice7* device, DWORD Type, DWORD Value);

// DirectDraw vtable indices
#define DDSURFACE7_VTABLE_FLIP 11
#define DDSURFACE7_VTABLE_BLT 5
#define DDSURFACE7_VTABLE_UNLOCK 32
#define DDSURFACE7_VTABLE_LOCK 25
#define DDSURFACE7_VTABLE_GETDC 17
#define DDSURFACE7_VTABLE_RELEASEDC 26
#define D3D7_VTABLE_SETRENDERSTATE 20
#define D3D7_VTABLE_ENDSCENE 6
#define D3D7_VTABLE_GETTEXTURESTAGESTATE 36
#define D3D7_VTABLE_SETTEXTURESTAGESTATE 37
#define D3D7_VTABLE_APPLYSTATEBLOCK 39
#define D3D6_VTABLE_ENDSCENE 10
#define D3D6_VTABLE_GETTEXTURESTAGESTATE 39
#define D3D6_VTABLE_SETTEXTURESTAGESTATE 40

// DirectDraw function typedefs
typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Flip_t)(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                     DWORD flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Flip_t)(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                     DWORD flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Blt_t)(IDirectDrawSurface7* surface, LPRECT destRect,
                                                    IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD flags,
                                                    void* bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Blt_t)(IDirectDrawSurface4* surface, LPRECT destRect,
                                                    IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD flags,
                                                    void* bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Unlock_t)(IDirectDrawSurface7* surface, LPRECT rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Unlock_t)(IDirectDrawSurface4* surface, LPRECT rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Lock_t)(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD flags, HANDLE event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Lock_t)(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD flags, HANDLE event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7GetDC_t)(IDirectDrawSurface7* surface, HDC* hdc);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7ReleaseDC_t)(IDirectDrawSurface7* surface, HDC hdc);
typedef HRESULT(STDMETHODCALLTYPE* DDraw7CreateSurface_t)(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                          IDirectDrawSurface7** ppSurface, IUnknown* pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDraw4CreateSurface_t)(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                          IDirectDrawSurface4** ppSurface, IUnknown* pUnkOuter);
typedef HRESULT(STDMETHODCALLTYPE* DDrawLegacyCreateSurface_t)(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                               IDirectDrawSurface** ppSurface, IUnknown* pUnkOuter);
typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyFlip_t)(IDirectDrawSurface* surface, IDirectDrawSurface* destOverride,
                                                          DWORD flags);
typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyBlt_t)(IDirectDrawSurface* surface, LPRECT destRect,
                                                         IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD flags,
                                                         DDBLTFX* bltFx);
typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyLock_t)(IDirectDrawSurface* surface, LPRECT destRect,
                                                          DDSURFACEDESC* surfaceDesc, DWORD flags, HANDLE event);
typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyUnlock_t)(IDirectDrawSurface* surface, LPVOID surfaceData);
typedef HRESULT(STDMETHODCALLTYPE* D3D7CreateDevice_t)(IDirect3D7*, REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**);
typedef HRESULT(STDMETHODCALLTYPE* D3D3CreateDevice_t)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**,
                                                       IUnknown*);

typedef HRESULT(WINAPI* DirectDrawCreate_t)(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* pUnkOuter);
typedef HRESULT(WINAPI* DirectDrawCreateEx_t)(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter);

// Original function pointers
static DDSurface7Flip_t oDDSurface7Flip = nullptr;
static DDSurface4Flip_t oDDSurface4Flip = nullptr;
static DDSurface7Blt_t oDDSurface7Blt = nullptr;
static DDSurface4Blt_t oDDSurface4Blt = nullptr;
static DDSurface7Lock_t oDDSurface7Lock = nullptr;
static DDSurface4Lock_t oDDSurface4Lock = nullptr;
static DDSurface7Unlock_t oDDSurface7Unlock = nullptr;
static DDSurface4Unlock_t oDDSurface4Unlock = nullptr;
static SetTextureStageState7_t oSetTextureStageState7 = nullptr;
static GetTextureStageState7_t oGetTextureStageState7 = nullptr;
static SetTextureStageState6_t oSetTextureStageState6 = nullptr;
static GetTextureStageState6_t oGetTextureStageState6 = nullptr;
static SetRenderState7_t oSetRenderState7 = nullptr;
static DirectDrawCreate_t oDirectDrawCreate = nullptr;
static DirectDrawCreateEx_t oDirectDrawCreateEx = nullptr;

// Globals
static PerformanceMetrics g_PerfMetrics;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static IDirectDrawSurface7* g_PrimarySurface = nullptr;
static IDirectDrawSurface4* g_PrimarySurface4 = nullptr;
static IDirectDrawSurface7* g_HookSurfacePrototype = nullptr;
static IDirectDrawSurface4* g_HookSurfacePrototype4 = nullptr;
static int g_CaptureRecurse = 0;
static std::vector<IDirectDrawSurface7*> g_PrerenderSurfaces;
static std::vector<void**> g_HookedDDrawVTables;
static std::vector<void**> g_HookedSurfaceVTables;
static uint32_t g_PrerenderIdx = 0;
static int64_t g_LastSleepUs = 0;
static IDirect3DDevice7* g_D3D7Device = nullptr;
static IDirectDrawSurface7* g_LastPresentedSourceSurface = nullptr;
static DWORD g_LastPresentedSourceTick = 0;
static bool g_DirectDrawCreateExInlineInstalled = false;
static bool g_DirectDrawCreateInlineInstalled = false;
static HHOOK g_DDrawBootstrapHook = nullptr;
static HWND g_DDrawBootstrapWindow = NULL;
static DWORD g_DDrawBootstrapThreadId = 0;
static std::atomic<bool> g_DDrawBootstrapQueued{false};
static std::atomic<bool> g_DDrawBootstrapRunning{false};
static thread_local unsigned g_DDrawBootstrapDepth = 0;
static std::atomic<int> g_ActiveDirectDrawVersion{0};
static std::atomic<unsigned> g_ActiveLegacyD3DVersion{0};
static std::atomic<unsigned> g_LegacyD3DCallbackVersion{0};

struct LegacyDDrawVTableRecord {
    DDrawLegacyCreateSurface_t createSurface = nullptr;
    ce::graphics_api_identity::DirectDrawVersion version = ce::graphics_api_identity::DirectDrawVersion::Unknown;
};

struct LegacySurfaceVTableRecord {
    DDSurfaceLegacyFlip_t flip = nullptr;
    DDSurfaceLegacyBlt_t blt = nullptr;
    DDSurfaceLegacyLock_t lock = nullptr;
    DDSurfaceLegacyUnlock_t unlock = nullptr;
};

static std::mutex g_DDrawIdentityMutex;
static std::unordered_map<void**, LegacyDDrawVTableRecord> g_LegacyDDrawVTables;
static std::unordered_map<void**, LegacySurfaceVTableRecord> g_LegacySurfaceVTables;
static std::unordered_map<void**, DDraw4CreateSurface_t> g_DDraw4CreateSurfaceOriginals;
static std::unordered_map<void**, DDraw7CreateSurface_t> g_DDraw7CreateSurfaceOriginals;
static std::unordered_map<void**, D3D7CreateDevice_t> g_D3D7CreateDeviceOriginals;
static std::unordered_map<void**, D3D3CreateDevice_t> g_D3D3CreateDeviceOriginals;
static std::unordered_map<uintptr_t, ce::graphics_api_identity::DirectDrawVersion> g_SurfaceDirectDrawVersions;
static std::unordered_map<uintptr_t, unsigned> g_SurfaceLegacyD3DVersions;

struct LegacyD3DSamplerVTableRecord {
    ce::legacy_d3d_sampler_state::Api api = ce::legacy_d3d_sampler_state::Api::D3D6;
    void** vtable = nullptr;
    std::atomic<ce::legacy_d3d_sampler_state::SetTextureStageStateFn> setState{nullptr};
    std::atomic<ce::legacy_d3d_sampler_state::GetTextureStageStateFn> getState{nullptr};
    std::atomic<LegacyD3DEndScene_t> endScene{nullptr};
    std::atomic<D3D7ApplyStateBlock_t> applyStateBlock{nullptr};
};

static std::mutex g_LegacyD3DSamplerVTableMutex;
static std::vector<std::unique_ptr<LegacyD3DSamplerVTableRecord>> g_LegacyD3DSamplerVTables;

class DirectDrawBootstrapScope {
public:
    DirectDrawBootstrapScope() {
        ++g_DDrawBootstrapDepth;
    }
    ~DirectDrawBootstrapScope() {
        --g_DDrawBootstrapDepth;
    }
};

static uintptr_t DirectDrawObjectIdentity(IUnknown* object) {
    if (!object)
        return 0;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity)
        return reinterpret_cast<uintptr_t>(object);
    const uintptr_t value = reinterpret_cast<uintptr_t>(identity);
    identity->Release();
    return value;
}

static void AssociateDirectDrawSurface(IUnknown* surface, ce::graphics_api_identity::DirectDrawVersion version) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    g_SurfaceDirectDrawVersions[identity] = version;
}

static void AssociateLegacyD3DSurface(IUnknown* surface, unsigned d3dVersion) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    g_SurfaceLegacyD3DVersions[identity] = d3dVersion;
}

static void ActivateDirectDrawSurface(IUnknown* surface, ce::graphics_api_identity::DirectDrawVersion fallbackVersion) {
    auto version = fallbackVersion;
    unsigned d3dVersion = 0;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto ddIt = g_SurfaceDirectDrawVersions.find(identity);
        if (ddIt != g_SurfaceDirectDrawVersions.end())
            version = ddIt->second;
        const auto d3dIt = g_SurfaceLegacyD3DVersions.find(identity);
        if (d3dIt != g_SurfaceLegacyD3DVersions.end())
            d3dVersion = d3dIt->second;
    }
    g_ActiveDirectDrawVersion.store(static_cast<int>(version), std::memory_order_release);
    if (d3dVersion == 0)
        d3dVersion = g_LegacyD3DCallbackVersion.load(std::memory_order_acquire);
    g_ActiveLegacyD3DVersion.store(d3dVersion, std::memory_order_release);
}

static UINT QueryD3D7MaxAnisotropy(void* opaqueDevice) {
    if (!opaqueDevice)
        return 1;
    auto* device = static_cast<IDirect3DDevice7*>(opaqueDevice);
    void** vtable = *(void***)device;
    using GetCaps7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, D3DDEVICEDESC7*);
    auto getCaps = reinterpret_cast<GetCaps7_t>(vtable[3]);
    D3DDEVICEDESC7 caps = {};
    return getCaps && SUCCEEDED(getCaps(device, &caps)) ? std::max<DWORD>(1, caps.dwMaxAnisotropy) : 1;
}

static UINT QueryD3D6MaxAnisotropy(void* opaqueDevice) {
    if (!opaqueDevice)
        return 1;
    auto* device = static_cast<IUnknown*>(opaqueDevice);
    void** vtable = *(void***)device;
    using GetCaps6_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, D3DDEVICEDESC*, D3DDEVICEDESC*);
    auto getCaps = reinterpret_cast<GetCaps6_t>(vtable[3]);
    D3DDEVICEDESC halCaps = {};
    D3DDEVICEDESC helCaps = {};
    halCaps.dwSize = sizeof(halCaps);
    helCaps.dwSize = sizeof(helCaps);
    if (!getCaps || FAILED(getCaps(device, &halCaps, &helCaps)))
        return 1;
    return std::max<DWORD>(1, halCaps.dwMaxAnisotropy ? halCaps.dwMaxAnisotropy : helCaps.dwMaxAnisotropy);
}

static bool ShouldSuppressDirectDrawHooking() {
    if (!IsDXVKD3D9WrapperLoaded()) {
        return false;
    }

    SharedMemoryLayout* shm = nullptr;
    if (g_IPC) {
        shm = g_IPC->GetSharedMem();
    }
    if (!shm) {
        shm = g_pSharedMem;
    }
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire)) {
        return false;
    }

    static std::atomic<int> s_suppressionLogCount{0};
    if (s_suppressionLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
        HookLogImportant("DDraw: DXVK d3d9 + Vulkan layer detected - suppressing DirectDraw bootstrap/hooks");
    }
    return true;
}

static bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& h);

static bool HasHookedVTable(const std::vector<void**>& hookedVTables, void** vtable) {
    return std::find(hookedVTables.begin(), hookedVTables.end(), vtable) != hookedVTables.end();
}

static bool IsPrimarySurfaceDesc(const DDSURFACEDESC2* surfaceDesc) {
    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
}

static bool IsPrimarySurfaceDesc(const DDSURFACEDESC* surfaceDesc) {
    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
}

static bool SurfaceHasCaps(IDirectDrawSurface* surface, DWORD capsMask) {
    if (!surface)
        return false;
    DDSCAPS caps = {};
    return SUCCEEDED(surface->GetCaps(&caps)) && (caps.dwCaps & capsMask) != 0;
}

static bool SurfaceHasCaps(IDirectDrawSurface7* surface, DWORD capsMask) {
    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;
}

static bool SurfaceHasCaps(IDirectDrawSurface4* surface, DWORD capsMask) {
    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;
}

static IDirectDrawSurface7* QuerySurface7(IUnknown* surfaceLike) {
    if (!surfaceLike)
        return nullptr;

    IDirectDrawSurface7* surface7 = nullptr;
    if (FAILED(surfaceLike->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&surface7)))) {
        return nullptr;
    }

    return surface7;
}

static void RememberPresentedSourceSurface(IDirectDrawSurface7* surface) {
    if (!surface)
        return;

    g_LastPresentedSourceSurface = surface;
    g_LastPresentedSourceTick = GetTickCount();
}

static IDirectDrawSurface7* ResolvePreferredPresentationSurface(IDirectDrawSurface7* primarySurface,
                                                                IDirectDrawSurface7* explicitSourceSurface) {
    uint32_t primaryWidth = 0;
    uint32_t primaryHeight = 0;
    const bool havePrimarySize = GetSurfaceSize(primarySurface, primaryWidth, primaryHeight);

    auto surfaceMatchesPrimary = [&](IDirectDrawSurface7* surface) {
        if (!surface)
            return false;
        if (!havePrimarySize)
            return true;
        uint32_t surfaceWidth = 0;
        uint32_t surfaceHeight = 0;
        return GetSurfaceSize(surface, surfaceWidth, surfaceHeight) && surfaceWidth == primaryWidth &&
               surfaceHeight == primaryHeight;
    };

    if (explicitSourceSurface && surfaceMatchesPrimary(explicitSourceSurface)) {
        return explicitSourceSurface;
    }

    const DWORD now = GetTickCount();
    if (g_LastPresentedSourceSurface && (now - g_LastPresentedSourceTick) <= 100 &&
        surfaceMatchesPrimary(g_LastPresentedSourceSurface)) {
        return g_LastPresentedSourceSurface;
    }

    return primarySurface;
}

static HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* pUnkOuter);
static HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* pUnkOuter);
static HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* pUnkOuter);
static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD flags);
static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD flags,
                                                          DDBLTFX* bltFx);
static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD flags, HANDLE event);
static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID surfaceData);
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD flags);
static HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD flags);
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx);
static HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx);
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD flags, HANDLE event);
static HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD flags, HANDLE event);
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT rect);
static HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT rect);
static HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* device, DWORD Type, DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD* pValue);
static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* device, DWORD Stage, DWORD Type, DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* device, DWORD Stage, DWORD Type, DWORD* pValue);
static HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* device);
static HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* device, DWORD blockHandle);
static HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* device);
static HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** device);
static HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** device,
                                                        IUnknown* outer);
static HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* pUnkOuter);
static HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter);

static void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* reason);
static void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* reason, bool markPrototype = false);
static void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* reason,
                                           bool markPrototype = false);
static void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* reason);
static void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* reason);
static void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* reason);
static void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* reason);
static void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* reason);
static void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t directDrawCreate);
static void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t directDrawCreateEx);
static void BootstrapDirectDrawHooksOnCurrentThread(const char* reason);
static bool QueueDirectDrawBootstrapOnWindowThread();

static LegacyD3DSamplerVTableRecord* ResolveLegacyD3DSamplerVTable(
    ce::legacy_d3d_sampler_state::Api api, void* device) {
    if (!device)
        return nullptr;

    void** vtable = *(void***)device;
    thread_local void** cachedD3D6VTable = nullptr;
    thread_local void** cachedD3D7VTable = nullptr;
    thread_local LegacyD3DSamplerVTableRecord* cachedD3D6Record = nullptr;
    thread_local LegacyD3DSamplerVTableRecord* cachedD3D7Record = nullptr;
    void**& cachedVTable = api == ce::legacy_d3d_sampler_state::Api::D3D7 ? cachedD3D7VTable : cachedD3D6VTable;
    LegacyD3DSamplerVTableRecord*& cachedRecord =
        api == ce::legacy_d3d_sampler_state::Api::D3D7 ? cachedD3D7Record : cachedD3D6Record;
    if (cachedVTable == vtable)
        return cachedRecord;

    std::lock_guard<std::mutex> lock(g_LegacyD3DSamplerVTableMutex);
    for (const auto& record : g_LegacyD3DSamplerVTables) {
        if (record->api == api && record->vtable == vtable) {
            cachedVTable = vtable;
            cachedRecord = record.get();
            return cachedRecord;
        }
    }
    return nullptr;
}

static void InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api api, void* device, bool newDevice,
                                        const char* reason) {
    if (!device)
        return;

    void** vtable = *(void***)device;
    LegacyD3DSamplerVTableRecord* record = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_LegacyD3DSamplerVTableMutex);
        for (const auto& candidate : g_LegacyD3DSamplerVTables) {
            if (candidate->api == api && candidate->vtable == vtable) {
                record = candidate.get();
                break;
            }
        }
        if (!record) {
            auto newRecord = std::make_unique<LegacyD3DSamplerVTableRecord>();
            newRecord->api = api;
            newRecord->vtable = vtable;
            record = newRecord.get();
            g_LegacyD3DSamplerVTables.push_back(std::move(newRecord));
        }

        const bool isD3D7 = api == ce::legacy_d3d_sampler_state::Api::D3D7;
        const size_t setSlot = isD3D7 ? D3D7_VTABLE_SETTEXTURESTAGESTATE : D3D6_VTABLE_SETTEXTURESTAGESTATE;
        const size_t getSlot = isD3D7 ? D3D7_VTABLE_GETTEXTURESTAGESTATE : D3D6_VTABLE_GETTEXTURESTAGESTATE;
        const size_t endSceneSlot = isD3D7 ? D3D7_VTABLE_ENDSCENE : D3D6_VTABLE_ENDSCENE;
        LPVOID setDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourSetTextureStageState7)
                                  : reinterpret_cast<LPVOID>(&DetourSetTextureStageState6);
        LPVOID getDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourGetTextureStageState7)
                                  : reinterpret_cast<LPVOID>(&DetourGetTextureStageState6);
        LPVOID endSceneDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourD3D7EndScene)
                                       : reinterpret_cast<LPVOID>(&DetourD3D6EndScene);

        if (!record->setState.load(std::memory_order_acquire)) {
            ce::legacy_d3d_sampler_state::SetTextureStageStateFn original = nullptr;
            if (VTableHook::Create(&vtable[setSlot], setDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->setState.store(original, std::memory_order_release);
                if (isD3D7 && !oSetTextureStageState7)
                    oSetTextureStageState7 = reinterpret_cast<SetTextureStageState7_t>(original);
                if (!isD3D7 && !oSetTextureStageState6)
                    oSetTextureStageState6 = reinterpret_cast<SetTextureStageState6_t>(original);
            }
        }
        if (!record->getState.load(std::memory_order_acquire)) {
            ce::legacy_d3d_sampler_state::GetTextureStageStateFn original = nullptr;
            if (VTableHook::Create(&vtable[getSlot], getDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->getState.store(original, std::memory_order_release);
                if (isD3D7 && !oGetTextureStageState7)
                    oGetTextureStageState7 = reinterpret_cast<GetTextureStageState7_t>(original);
                if (!isD3D7 && !oGetTextureStageState6)
                    oGetTextureStageState6 = reinterpret_cast<GetTextureStageState6_t>(original);
            }
        }
        if (!record->endScene.load(std::memory_order_acquire)) {
            LegacyD3DEndScene_t original = nullptr;
            if (VTableHook::Create(&vtable[endSceneSlot], endSceneDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->endScene.store(original, std::memory_order_release);
            }
        }
        if (isD3D7 && !record->applyStateBlock.load(std::memory_order_acquire)) {
            D3D7ApplyStateBlock_t original = nullptr;
            if (VTableHook::Create(&vtable[D3D7_VTABLE_APPLYSTATEBLOCK],
                                   reinterpret_cast<LPVOID>(&DetourD3D7ApplyStateBlock),
                                   reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
                record->applyStateBlock.store(original, std::memory_order_release);
            }
        }
    }

    auto queryMaxAnisotropy = api == ce::legacy_d3d_sampler_state::Api::D3D7 ? QueryD3D7MaxAnisotropy
                                                                              : QueryD3D6MaxAnisotropy;
    ce::legacy_d3d_sampler_state::RegisterDevice(api, device, newDevice, queryMaxAnisotropy);
    HookLog("DDraw: DX%u sampler hooks reconciled vtable=%p reason=%s", api == ce::legacy_d3d_sampler_state::Api::D3D7 ? 7u : 6u,
            vtable, reason ? reason : "unknown");
}

static HWND ResolveDirectDrawTargetWindow() {
    if (g_CachedHwnd && IsWindow(g_CachedHwnd)) {
        return g_CachedHwnd;
    }

    if (g_DDrawBootstrapWindow && IsWindow(g_DDrawBootstrapWindow)) {
        return g_DDrawBootstrapWindow;
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

static void MaybeTrackPrimarySurface(IDirectDrawSurface7* surface, const char* reason) {
    if (!surface || surface == g_HookSurfacePrototype || g_PrimarySurface)
        return;

    g_PrimarySurface = surface;
    HookLog("DDraw: Tracking runtime primary surface from %s (%p)", reason, surface);
}

static void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface, const char* reason) {
    if (!surface || surface == g_HookSurfacePrototype4 || g_PrimarySurface4)
        return;

    g_PrimarySurface4 = surface;
    HookLog("DDraw: Tracking runtime primary surface4 from %s (%p)", reason, surface);
}

static void ApplyPrerenderLimitDDraw(IDirectDrawSurface7* surface, float limit) {
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

        if (g_PrerenderSurfaces.size() != (size_t)lookback) {
            g_PrerenderSurfaces.assign(lookback, nullptr);
            g_PrerenderIdx = 0;
        }
