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

typedef HRESULT(STDMETHODCALLTYPE* SetRenderState7_t)(IDirect3DDevice7* device, DWORD Type, DWORD Value);

// DirectDraw vtable indices
#define DDSURFACE7_VTABLE_FLIP 11
#define DDSURFACE7_VTABLE_BLT 5
#define DDSURFACE7_VTABLE_UNLOCK 32
#define DDSURFACE7_VTABLE_LOCK 25
#define DDSURFACE7_VTABLE_GETDC 17
#define DDSURFACE7_VTABLE_RELEASEDC 26
#define D3D7_VTABLE_SETRENDERSTATE 20
#define D3D7_VTABLE_GETTEXTURESTAGESTATE 36
#define D3D7_VTABLE_SETTEXTURESTAGESTATE 37
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

        uint32_t waitIdx = g_PrerenderIdx % (uint32_t)g_PrerenderSurfaces.size();
        if (g_PrerenderSurfaces[waitIdx]) {
            IDirectDrawSurface7* waitSurf = g_PrerenderSurfaces[waitIdx];
            typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
            void** vtable = *(void***)waitSurf;
            GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];

            while (pGetFlipStatus(waitSurf, 1) == 0x887600FA) {
                std::this_thread::yield();
            }
        }

        g_PrerenderSurfaces[waitIdx] = surface;
        g_PrerenderIdx++;
    }

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = g_PerfMetrics.GetCurrentFPS();
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

static void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t directDrawCreate) {
    if (!directDrawCreate || g_DirectDrawCreateInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)directDrawCreate, (void*)DetourDirectDrawCreate, &trampoline)) {
        oDirectDrawCreate = reinterpret_cast<DirectDrawCreate_t>(trampoline);
        g_DirectDrawCreateInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreate inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreate inline hook failed");
    }
}

static void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t directDrawCreateEx) {
    if (!directDrawCreateEx || g_DirectDrawCreateExInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)directDrawCreateEx, (void*)DetourDirectDrawCreateEx, &trampoline)) {
        oDirectDrawCreateEx = reinterpret_cast<DirectDrawCreateEx_t>(trampoline);
        g_DirectDrawCreateExInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreateEx inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreateEx inline hook failed");
    }
}

static bool FindDirectDrawBootstrapWindow(HWND* outWindow, DWORD* outThreadId) {
    if (!outWindow || !outThreadId)
        return false;

    *outWindow = NULL;
    *outThreadId = 0;

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    DWORD foregroundThreadId = 0;
    if (foregroundWindow) {
        foregroundThreadId = GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        if (foregroundPid == GetCurrentProcessId()) {
            *outWindow = foregroundWindow;
            *outThreadId = foregroundThreadId;
            return true;
        }
    }

    ce::overlay_compat::AuxiliaryProcessWindowInfo info = {};
    if (ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), nullptr, &info) && info.hwnd &&
        info.threadId != 0) {
        *outWindow = info.hwnd;
        *outThreadId = info.threadId;
        return true;
    }

    return false;
}

static LRESULT CALLBACK DirectDrawBootstrapHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && g_DDrawBootstrapQueued.load(std::memory_order_acquire) &&
        !g_DDrawBootstrapRunning.exchange(true, std::memory_order_acq_rel)) {
        HHOOK hook = g_DDrawBootstrapHook;
        g_DDrawBootstrapHook = nullptr;
        g_DDrawBootstrapQueued.store(false, std::memory_order_release);
        if (hook) {
            UnhookWindowsHookEx(hook);
        }

        BootstrapDirectDrawHooksOnCurrentThread("window-thread bootstrap");
        g_DDrawBootstrapRunning.store(false, std::memory_order_release);
    }

    return CallNextHookEx(g_DDrawBootstrapHook, code, wParam, lParam);
}

static bool QueueDirectDrawBootstrapOnWindowThread() {
    if (g_HooksInitialized)
        return true;

    HWND bootstrapWindow = NULL;
    DWORD bootstrapThreadId = 0;
    if (!FindDirectDrawBootstrapWindow(&bootstrapWindow, &bootstrapThreadId) || !bootstrapWindow ||
        bootstrapThreadId == 0) {
        HookLog("DDraw: Failed to find bootstrap window thread");
        return false;
    }

    if (g_DDrawBootstrapHook) {
        HookLog("DDraw: Bootstrap window hook already queued (hwnd=%p, tid=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId);
        return true;
    }

    g_DDrawBootstrapWindow = bootstrapWindow;
    g_DDrawBootstrapThreadId = bootstrapThreadId;
    g_DDrawBootstrapHook = SetWindowsHookExA(WH_CALLWNDPROC, DirectDrawBootstrapHookProc, NULL, bootstrapThreadId);
    if (!g_DDrawBootstrapHook) {
        HookLog("DDraw: Failed to install bootstrap window hook (hwnd=%p, tid=%lu, err=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId, GetLastError());
        return false;
    }

    g_DDrawBootstrapQueued.store(true, std::memory_order_release);
    HookLog("DDraw: Queued bootstrap window hook (hwnd=%p, tid=%lu)", bootstrapWindow,
            (unsigned long)bootstrapThreadId);

    DWORD_PTR sendResult = 0;
    if (!SendMessageTimeoutA(bootstrapWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &sendResult)) {
        HookLog("DDraw: Failed to send bootstrap wake message (hwnd=%p, tid=%lu, err=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId, GetLastError());
    }

    return true;
}

static void BootstrapDirectDrawHooksOnCurrentThread(const char* reason) {
    if (g_HooksInitialized)
        return;
    DirectDrawBootstrapScope bootstrapScope;

    HookLog("DDraw: BootstrapDirectDrawHooksOnCurrentThread starting via %s", reason);

    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        HookLog("DDraw: ddraw.dll not loaded during bootstrap");
        return;
    }

    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found during bootstrap");
        return;
    }
    DirectDrawCreate_t pDirectDrawCreate = (DirectDrawCreate_t)GetProcAddress(ddrawModule, "DirectDrawCreate");
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);

    DirectDrawCreateEx_t createFunction = oDirectDrawCreateEx ? oDirectDrawCreateEx : pDirectDrawCreateEx;
    HookLog("DDraw: Bootstrap create function=%p (export=%p, trampoline=%p)", createFunction, pDirectDrawCreateEx,
            oDirectDrawCreateEx);

    IDirectDraw7* ddraw7 = nullptr;
    HRESULT hr = createFunction(NULL, (LPVOID*)&ddraw7, IID_IDirectDraw7, NULL);
    if (FAILED(hr) || !ddraw7) {
        HookLog("DDraw: Failed to create DirectDraw7 (hr=0x%08x)", hr);
        InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);
        return;
    }
    HookLog("DDraw: Bootstrap DirectDraw7 created (object=%p)", ddraw7);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DDrawDummyClass";
    RegisterClassExA(&wc);

    HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "DDrawDummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                     wc.hInstance, NULL);

    hr = ddraw7->SetCooperativeLevel(dummyHwnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        HookLog("DDraw: SetCooperativeLevel failed during bootstrap (hr=0x%08x)", hr);
    }

    InstallDirectDrawHooksForInstance(ddraw7, reason);

    DDSURFACEDESC2 ddsd = {};
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    IDirectDrawSurface7* dummySurface = nullptr;
    hr = ddraw7->CreateSurface(&ddsd, &dummySurface, NULL);
    HookLog("DDraw: Bootstrap CreateSurface returned hr=0x%08x, surface=%p", hr, dummySurface);

    if (SUCCEEDED(hr) && dummySurface) {
        InstallSurfaceHooksForSurface(dummySurface, reason, true);

        IDirect3D7* d3d7 = nullptr;
        if (SUCCEEDED(ddraw7->QueryInterface(kIID_IDirect3D7, (void**)&d3d7))) {
            IDirect3DDevice7* d3d7Device = nullptr;
            if (SUCCEEDED(d3d7->CreateDevice(kIID_IDirect3DHALDevice, dummySurface, &d3d7Device))) {
                void** d3d7DeviceVTable = *(void***)d3d7Device;

                if (VTableHook::Create(&d3d7DeviceVTable[D3D7_VTABLE_SETTEXTURESTAGESTATE],
                                       (LPVOID)&DetourSetTextureStageState7,
                                       (LPVOID*)&oSetTextureStageState7) == VTableHook::Success) {
                    HookLog("DDraw: SetTextureStageState hook installed");
                }
                if (VTableHook::Create(&d3d7DeviceVTable[D3D7_VTABLE_GETTEXTURESTAGESTATE],
                                       (LPVOID)&DetourGetTextureStageState7,
                                       (LPVOID*)&oGetTextureStageState7) == VTableHook::Success) {
                    HookLog("DDraw: Logical GetTextureStageState hook installed");
                }

                if (VTableHook::Create(&d3d7DeviceVTable[D3D7_VTABLE_SETRENDERSTATE], (LPVOID)&DetourSetRenderState7,
                                       (LPVOID*)&oSetRenderState7) == VTableHook::Success) {
                    HookLog("DDraw: SetRenderState hook installed");
                }

                d3d7Device->Release();
            } else {
                HookLog("DDraw: Failed to create D3D7 device for hooking");
            }
            d3d7->Release();
        }

        IDirectDrawSurface4* dummySurface4 = nullptr;
        IUnknown* d3d3 = nullptr;
        if (SUCCEEDED(dummySurface->QueryInterface(IID_IDirectDrawSurface4, (void**)&dummySurface4)) &&
            SUCCEEDED(ddraw7->QueryInterface(kIID_IDirect3D3, (void**)&d3d3))) {
            using CreateDevice3_t =
                HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**, IUnknown*);
            void** d3d3VTable = *(void***)d3d3;
            auto createDevice3 = reinterpret_cast<CreateDevice3_t>(d3d3VTable[8]);
            IUnknown* d3d6Device = nullptr;
            if (createDevice3 &&
                SUCCEEDED(createDevice3(d3d3, kIID_IDirect3DHALDevice, dummySurface4, &d3d6Device, nullptr))) {
                void** deviceVTable = *(void***)d3d6Device;
                if (VTableHook::Create(&deviceVTable[D3D6_VTABLE_SETTEXTURESTAGESTATE],
                                       (LPVOID)&DetourSetTextureStageState6,
                                       (LPVOID*)&oSetTextureStageState6) == VTableHook::Success) {
                    HookLog("DDraw: DX6 SetTextureStageState hook installed");
                }
                if (VTableHook::Create(&deviceVTable[D3D6_VTABLE_GETTEXTURESTAGESTATE],
                                       (LPVOID)&DetourGetTextureStageState6,
                                       (LPVOID*)&oGetTextureStageState6) == VTableHook::Success) {
                    HookLog("DDraw: DX6 logical GetTextureStageState hook installed");
                }
                d3d6Device->Release();
            } else {
                HookLog("DDraw: Failed to create D3D6 device for sampler hooking");
            }
        }
        if (d3d3)
            d3d3->Release();
        if (dummySurface4)
            dummySurface4->Release();
    } else {
        HookLog("DDraw: Failed to create primary surface (hr=0x%08x)", hr);
    }

    ddraw7->Release();
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);
    InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);

    DestroyWindow(dummyHwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    g_HooksInitialized = true;
    HookLog("DDrawHook: Hooks installed");
}

// DirectDraw Capture class
class DDrawCapture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // D3D9Ex wrapper for GPU sharing
    IDirect3D9Ex* d3d9Ex = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;
    IDirect3DSurface9* d3d9FastUploadSurface = nullptr;
    IDirect3DSurface9* d3d9UploadSurface = nullptr;
    bool d3d9UsesFlipEx = false;

    // D3D11 for shared texture
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Surface info
    IDirectDrawSurface7* ddrawSurface = nullptr;
    HWND targetHwnd = NULL;

    void ReleaseOverlayResources() {
        if (d3d9FastUploadSurface) {
            d3d9FastUploadSurface->Release();
            d3d9FastUploadSurface = nullptr;
        }
        if (d3d9UploadSurface) {
            d3d9UploadSurface->Release();
            d3d9UploadSurface = nullptr;
        }
        if (d3d9DeviceEx) {
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
        }
        if (d3d9Ex) {
            d3d9Ex->Release();
            d3d9Ex = nullptr;
        }
        d3d9UsesFlipEx = false;
    }

    void Cleanup() override {
        CleanupDDraw(false);
    }

    bool CleanupDDraw(bool force = false) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        bool hasPublishedGeneration = sharedFenceHandle.load(std::memory_order_acquire) != NULL;
        for (const auto& handle : sharedTextureHandles)
            hasPublishedGeneration = hasPublishedGeneration || handle.load(std::memory_order_acquire) != NULL;
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("DDraw: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }

        // Release D3D11 resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HANDLE sharedHandle = sharedTextureHandles[i].exchange(NULL, std::memory_order_acq_rel);
            if (sharedHandle && sharedTextureHandleOwned[i].exchange(false, std::memory_order_acq_rel))
                CloseHandle(sharedHandle);
            if (sharedTextures[i]) {
                sharedTextures[i]->Release();
                sharedTextures[i] = nullptr;
            }
        }

        if (stagingTexture) {
            stagingTexture->Release();
            stagingTexture = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (context4) {
            context4->Release();
            context4 = nullptr;
        }
        if (sharedFenceHandle) {
            CloseHandle(sharedFenceHandle);
            sharedFenceHandle = NULL;
        }

        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }

        ReleaseOverlayResources();

        ddrawSurface = nullptr;
        targetHwnd = NULL;
        initialized = false;
        useFences = false;
        fenceValue = 0;
        return true;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            HookLog("DDraw: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("DDraw: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                        &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get adapter LUID
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Try to get context4 for fences
        if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            ID3D11Device5* device5 = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
                if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                    HANDLE hTemp = NULL;
                    fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hTemp);
                    sharedFenceHandle.store(hTemp, std::memory_order_release);
                    useFences = true;
                    HookLog("DDraw: D3D11.3 fence sync enabled");
                }
                device5->Release();
            }
        }

        HookLog("DDraw: D3D11 device created (LUID: %08x)", luidLow);
        return true;
    }

    bool CreateStagingTexture() {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DYNAMIC;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &stagingTexture);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create staging texture (hr=0x%08x)", hr);
            return false;
        }

        return true;
    }

    bool CreateSharedTextures() {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr)) {
                HookLog("DDraw: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            HANDLE hTemp = NULL;
            resource->GetSharedHandle(&hTemp);
            sharedTextureHandles[i].store(hTemp, std::memory_order_release);
            resource->Release();
        }

        HookLog("DDraw: Shared textures created");
        return true;
    }

    bool CreateD3D9ExWrapper(HWND hwnd) {
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9) {
            HookLog("DDraw: D3D9 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        PFN_Direct3DCreate9Ex pDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

        if (!pDirect3DCreate9Ex) {
            HookLog("DDraw: Direct3DCreate9Ex not found");
            return false;
        }

        HRESULT hr = pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex (hr=0x%08x)", hr);
            return false;
        }

        // The DirectDraw app already controls frame pacing on its own presentation path.
        // The helper swap chain should avoid introducing a second vsync throttle.
        UINT presentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        auto tryCreateDevice = [&](D3DSWAPEFFECT swapEffect, UINT backBufferCount) {
            D3DPRESENT_PARAMETERS d3dpp = {};
            d3dpp.Windowed = TRUE;
            d3dpp.SwapEffect = swapEffect;
            d3dpp.hDeviceWindow = hwnd;
            d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
            d3dpp.BackBufferWidth = width;
            d3dpp.BackBufferHeight = height;
            d3dpp.BackBufferCount = backBufferCount;
            d3dpp.PresentationInterval = presentationInterval;
            return d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                          D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &d3dpp, NULL,
                                          &d3d9DeviceEx);
        };

        hr = tryCreateDevice(D3DSWAPEFFECT_FLIPEX, 2);
        if (SUCCEEDED(hr)) {
            d3d9UsesFlipEx = true;
        } else {
            hr = tryCreateDevice(D3DSWAPEFFECT_DISCARD, 1);
            d3d9UsesFlipEx = false;
        }

        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            return false;
        }

        LUID helperLuid = {};
        const HRESULT luidHr = d3d9Ex->GetAdapterLUID(D3DADAPTER_DEFAULT, &helperLuid);
        if (SUCCEEDED(luidHr) && (helperLuid.LowPart != 0 || helperLuid.HighPart != 0)) {
            luidLow = helperLuid.LowPart;
            luidHigh = helperLuid.HighPart;
            ReportLUID(luidLow, luidHigh);
            HookLog("DDraw: Published D3D9Ex overlay-helper LUID %08x:%08x", luidHigh, luidLow);
        } else {
            HookLog("DDraw: D3D9Ex overlay-helper LUID unavailable (hr=0x%08x)", luidHr);
        }

        HookLog("DDraw: Created D3D9Ex helper device with %s swap effect", d3d9UsesFlipEx ? "FLIPEX" : "DISCARD");

        d3d9DeviceEx->SetMaximumFrameLatency(1);

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                       &d3d9FastUploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create fast D3D9Ex upload surface (hr=0x%08x)", hr);
        }

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &d3d9UploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex upload surface (hr=0x%08x)", hr);
            return false;
        }

        HookLog("DDraw: D3D9Ex wrapper created for overlay");
        return true;
    }

    bool UploadOverlaySurfaceToBackbuffer() {
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int backBufferFailLogCount = 0;
            if (backBufferFailLogCount < 4) {
                HookLog("DDraw: Failed to get helper backbuffer for overlay composite (hr=0x%08x)", hr);
                backBufferFailLogCount++;
            }
            return false;
        }

        hr = d3d9DeviceEx->UpdateSurface(d3d9UploadSurface, nullptr, backBuffer, nullptr);
        backBuffer->Release();

        if (FAILED(hr)) {
            static int updateSurfaceFailLogCount = 0;
            if (updateSurfaceFailLogCount < 4) {
                HookLog("DDraw: Failed to upload DD surface into helper backbuffer (hr=0x%08x)", hr);
                updateSurfaceFailLogCount++;
            }
            return false;
        }

        return true;
    }

    bool StretchOverlaySurfaceToBackbuffer(IDirect3DSurface9* surface) {
        if (!surface) {
            return false;
        }

        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int backBufferFailLogCount = 0;
            if (backBufferFailLogCount < 4) {
                HookLog("DDraw: Failed to get helper backbuffer for fast overlay composite (hr=0x%08x)", hr);
                backBufferFailLogCount++;
            }
            return false;
        }

        hr = d3d9DeviceEx->StretchRect(surface, nullptr, backBuffer, nullptr, D3DTEXF_NONE);
        backBuffer->Release();

        if (FAILED(hr)) {
            static int stretchRectFailLogCount = 0;
            if (stretchRectFailLogCount < 4) {
                HookLog("DDraw: Failed to stretch DD surface into helper backbuffer (hr=0x%08x)", hr);
                stretchRectFailLogCount++;
            }
            return false;
        }

        return true;
    }

    bool CopyLockedSurfaceToUploadSurface(const DDSURFACEDESC2& desc) {
        if (!desc.lpSurface || desc.dwWidth != width || desc.dwHeight != height ||
            desc.ddpfPixelFormat.dwRGBBitCount != 32) {
            return false;
        }

        if (d3d9FastUploadSurface) {
            D3DLOCKED_RECT fastLockedRect = {};
            HRESULT fastHr = d3d9FastUploadSurface->LockRect(&fastLockedRect, nullptr, 0);
            if (SUCCEEDED(fastHr)) {
                const uint8_t* src = static_cast<const uint8_t*>(desc.lpSurface);
                uint8_t* dst = static_cast<uint8_t*>(fastLockedRect.pBits);
                const size_t rowBytes = static_cast<size_t>(width) * 4u;
                for (uint32_t y = 0; y < height; ++y) {
                    memcpy(dst, src, rowBytes);
                    src += desc.lPitch;
                    dst += fastLockedRect.Pitch;
                }

                d3d9FastUploadSurface->UnlockRect();
                if (StretchOverlaySurfaceToBackbuffer(d3d9FastUploadSurface)) {
                    return true;
                }
            }
        }

        D3DLOCKED_RECT lockedRect = {};
        HRESULT hr = d3d9UploadSurface->LockRect(&lockedRect, nullptr, 0);
        if (FAILED(hr)) {
            static int uploadLockFailLogCount = 0;
            if (uploadLockFailLogCount < 4) {
                HookLog("DDraw: Failed to lock D3D9 upload surface for overlay composite (hr=0x%08x)", hr);
                uploadLockFailLogCount++;
            }
            return false;
        }

        const uint8_t* src = static_cast<const uint8_t*>(desc.lpSurface);
        uint8_t* dst = static_cast<uint8_t*>(lockedRect.pBits);
        const size_t rowBytes = static_cast<size_t>(width) * 4u;
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(dst, src, rowBytes);
            src += desc.lPitch;
            dst += lockedRect.Pitch;
        }

        d3d9UploadSurface->UnlockRect();
        return UploadOverlaySurfaceToBackbuffer();
    }

    bool CopySurfaceToOverlayBackbufferViaLock(IDirectDrawSurface7* surface) {
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
        if (FAILED(hr) || !desc.lpSurface) {
            return false;
        }

        const bool copied = CopyLockedSurfaceToUploadSurface(desc);
        surface->Unlock(nullptr);
        return copied;
    }

    bool CopyPrimarySurfaceToOverlayBackbuffer(IDirectDrawSurface7* surface) {
        if (!surface || !d3d9DeviceEx || !d3d9UploadSurface || width == 0 || height == 0) {
            return false;
        }

        if (CopySurfaceToOverlayBackbufferViaLock(surface)) {
            return true;
        }

        HDC sourceDC = nullptr;
        HRESULT hr = surface->GetDC(&sourceDC);
        if (FAILED(hr) || !sourceDC) {
            static int sourceDcFailLogCount = 0;
            if (sourceDcFailLogCount < 4) {
                HookLog("DDraw: Failed to get source surface DC for overlay composite (hr=0x%08x)", hr);
                sourceDcFailLogCount++;
            }
            return false;
        }

        HDC uploadDC = nullptr;
        hr = d3d9UploadSurface->GetDC(&uploadDC);
        if (FAILED(hr) || !uploadDC) {
            surface->ReleaseDC(sourceDC);
            static int uploadDcFailLogCount = 0;
            if (uploadDcFailLogCount < 4) {
                HookLog("DDraw: Failed to get D3D9 upload DC for overlay composite (hr=0x%08x)", hr);
                uploadDcFailLogCount++;
            }
            return false;
        }

        BOOL bitBltOk =
            BitBlt(uploadDC, 0, 0, static_cast<int>(width), static_cast<int>(height), sourceDC, 0, 0, SRCCOPY);

        d3d9UploadSurface->ReleaseDC(uploadDC);
        surface->ReleaseDC(sourceDC);

        if (!bitBltOk) {
            static int bitBltFailLogCount = 0;
            if (bitBltFailLogCount < 4) {
                HookLog("DDraw: BitBlt into overlay upload surface failed (err=%lu)", GetLastError());
                bitBltFailLogCount++;
            }
            return false;
        }

        return UploadOverlaySurfaceToBackbuffer();
    }

    bool EnsureOverlayDevice(HWND hwnd, uint32_t w, uint32_t h) {
        if (!hwnd || w == 0 || h == 0) {
            static int invalidOverlayStateLogCount = 0;
            if (invalidOverlayStateLogCount < 3) {
                HookLog("DDraw: EnsureOverlayDevice skipped (hwnd=%p, size=%ux%u)", hwnd, w, h);
                invalidOverlayStateLogCount++;
            }
            return false;
        }

        const bool hwndChanged = targetHwnd && hwnd != targetHwnd;
        const bool sizeChanged = width != w || height != h;
        if (initialized && (hwndChanged || sizeChanged)) {
            // Capture owns the generation-wide dimensions. Let
            // EnsureCaptureResources drain/rebuild it before mutating them.
            return false;
        }
        if ((hwndChanged || sizeChanged) && d3d9DeviceEx) {
            HookLog("DDraw: Recreating overlay helper (oldHwnd=%p newHwnd=%p old=%ux%u new=%ux%u)", targetHwnd, hwnd,
                    width, height, w, h);
            ReleaseOverlayResources();
        }

        targetHwnd = hwnd;
        width = w;
        height = h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (d3d9DeviceEx) {
            return true;
        }

        if (!CreateD3D9ExWrapper(hwnd)) {
            HookLog("DDraw: Overlay disabled (D3D9Ex wrapper failed)");
            return false;
        }

        HookLog("DDraw: Overlay helper ready (hwnd=%p, size=%ux%u)", hwnd, width, height);
        return true;
    }

    bool EnsureCaptureResources(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t h) {
        if (!surface || w == 0 || h == 0) {
            HookLog("DDraw: EnsureCaptureResources skipped (surface=%p, size=%ux%u)", surface, w, h);
            return false;
        }

        if (initialized && ddrawSurface == surface && width == w && height == h) {
            if (hwnd) {
                targetHwnd = hwnd;
            }
            return true;
        }

        if (initialized) {
            HookLog(
                "DDraw: Reinitializing capture resources for new surface/size (oldSurface=%p newSurface=%p old=%ux%u "
                "new=%ux%u)",
                ddrawSurface, surface, width, height, w, h);
            if (!CleanupDDraw(false))
                return false;
        }

        ddrawSurface = surface;
        targetHwnd = hwnd;
        width = w;
        height = h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (!CreateD3D11Device()) {
            CleanupDDraw(false);
            return false;
        }

        if (!CreateStagingTexture()) {
            CleanupDDraw(false);
            return false;
        }

        if (!CreateSharedTextures()) {
            CleanupDDraw(false);
            return false;
        }

        EnsureOverlayDevice(hwnd, w, h);

        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DDraw Capture Initialized: %dx%d", width, height);
        return true;
    }

    bool PresentOverlay() {
        if (!d3d9DeviceEx) {
            return false;
        }

        HWND presentWindowOverride = d3d9UsesFlipEx ? nullptr : targetHwnd;
        HRESULT hr = d3d9DeviceEx->PresentEx(nullptr, nullptr, presentWindowOverride, nullptr, 0);
        static uint32_t overlayPresentCount = 0;
        overlayPresentCount++;
        if (overlayPresentCount <= 8) {
            HookLogImportant("DDraw: Overlay helper PresentEx hr=0x%08X hwnd=%p size=%ux%u count=%u", (unsigned)hr,
                             targetHwnd, width, height, overlayPresentCount);
        }

        if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING) {
            HookLog("DDraw: Overlay helper present failed (hr=0x%08x)", hr);
        }

        return SUCCEEDED(hr);
    }

    bool CaptureFrameFromSurface(IDirectDrawSurface7* surface) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!surface) {
            return false;
        }

        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        HRESULT hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
        if (SUCCEEDED(hr) && desc.lpSurface && desc.ddpfPixelFormat.dwRGBBitCount == 32 && desc.dwWidth == width &&
            desc.dwHeight == height) {
            CaptureFrame(desc.lpSurface, desc.lPitch);
            surface->Unlock(nullptr);
            return true;
        }

        if (SUCCEEDED(hr)) {
            surface->Unlock(nullptr);
        }

        CaptureFrameViaGDI(surface);
        return true;
    }

    void Init(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t h) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        EnsureCaptureResources(surface, hwnd, w, h);
    }

    void CaptureFrame(void* bits, int pitch) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!initialized || !bits)
            return;

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int idx = FindAvailableCaptureTextureSlot(captureSharedMem, writeIndex.load(std::memory_order_relaxed));
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeIndex.store(idx, std::memory_order_relaxed);

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

        // Map staging texture and copy from DDraw surface
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3d11Context->Map(stagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData)
            return;

        // Copy row by row (handle different pitches)
        uint8_t* src = (uint8_t*)bits;
        uint8_t* dst = (uint8_t*)mapped.pData;
        int rowSize = width * 4;  // Assuming 32-bit color

        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst, src, rowSize);
            src += pitch;
            dst += mapped.RowPitch;
        }

        d3d11Context->Unmap(stagingTexture, 0);

        // Copy staging to shared texture
        d3d11Context->CopyResource(sharedTextures[idx], stagingTexture);

        // Signal fence if available
        uint64_t publishedFenceValue = 0;
        if (useFences && context4 && fence) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DDraw: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
                        static_cast<unsigned long long>(candidateFenceValue), signalHr);
                useFences = false;
            }
        }
        if (publishedFenceValue == 0)
            d3d11Context->Flush();

        // PASS RAW QPC
        SignalFrameReady(g_IPC, idx, qpc.QuadPart, publishedFenceValue);
        AdvanceWriteIndex();
    }

    // Capture via GetDC for surfaces that don't support Lock
    void CaptureFrameViaGDI(IDirectDrawSurface7* surface) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (!initialized)
            return;

        HDC hdc = NULL;
        if (FAILED(surface->GetDC(&hdc)) || !hdc)
            return;

        // Create compatible DC and bitmap
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -(int)height;  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ oldBm = SelectObject(memDC, hbm);

        // BitBlt from surface DC
        BitBlt(memDC, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

        // Capture the bits
        CaptureFrame(bits, width * 4);

        // Cleanup
        SelectObject(memDC, oldBm);
        DeleteObject(hbm);
        DeleteDC(memDC);

        surface->ReleaseDC(hdc);
    }
};

static DDrawCapture g_DDrawCapture;

// Draw overlay using D3D9Ex
static void DrawDDrawOverlay(IDirectDrawSurface7* overlaySourceSurface) {
    if (!g_DDrawCapture.d3d9DeviceEx)
        return;

    if (g_DDrawCapture.targetHwnd && g_DDrawCapture.targetHwnd != g_CachedHwnd) {
        g_CachedHwnd = g_DDrawCapture.targetHwnd;
        InputManager::Get().HookWindow(g_CachedHwnd);
    }

    if (g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(g_CachedHwnd);
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        g_CachedHwnd = g_DDrawCapture.targetHwnd;
        if (g_CachedHwnd) {
            InputManager::Get().HookWindow(g_CachedHwnd);
            g_OverlayAdapter.SetHwnd(g_CachedHwnd);
        }
        if (g_OverlayAdapter.InitDX9(g_DDrawCapture.d3d9DeviceEx)) {
            if (g_CachedHwnd) {
                g_OverlayAdapter.SetHwnd(g_CachedHwnd);
            }
            HookLog("DDraw: OverlayAdapter initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DDrawCapture.droppedFrames.load(std::memory_order_relaxed));
    const auto directDrawVersion = static_cast<ce::graphics_api_identity::DirectDrawVersion>(
        g_ActiveDirectDrawVersion.load(std::memory_order_acquire));
    const unsigned d3dVersion = g_ActiveLegacyD3DVersion.load(std::memory_order_acquire);
    g_OverlayAdapter.SetGraphicsAPI(ce::graphics_api_identity::LegacyDirectXLabel(directDrawVersion, d3dVersion),
                                    "active DirectDraw presentation surface");

    if (g_OverlayAdapter.IsInitialized() && g_DDrawCapture.width > 0 && g_DDrawCapture.height > 0) {
        g_DDrawCapture.CopyPrimarySurfaceToOverlayBackbuffer(overlaySourceSurface);
        g_OverlayAdapter.RenderOverlay(g_DDrawCapture.width, g_DDrawCapture.height);
        static uint32_t overlayRenderSubmitCount = 0;
        overlayRenderSubmitCount++;
        if (overlayRenderSubmitCount <= 8) {
            HookLogImportant("DDraw: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)",
                             g_DDrawCapture.targetHwnd, g_DDrawCapture.width, g_DDrawCapture.height,
                             overlayRenderSubmitCount);
        }
        g_DDrawCapture.PresentOverlay();
    }
}

static void InstallAttachedBackBufferHooks(IDirectDrawSurface7* primarySurface, const char* reason) {
    if (!primarySurface) {
        return;
    }

    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer) {
        InstallSurfaceHooksForSurface(backBuffer, reason);
        backBuffer->Release();
    }
}

// Get surface dimensions from DDSURFACEDESC2
static bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& h) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);

    if (surface && SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
        w = desc.dwWidth;
        h = desc.dwHeight;
        return true;
    }
    return false;
}

static void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* reason) {
    if (!surface)
        return;
    void** vtable = *(void***)surface;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    if (g_LegacySurfaceVTables.find(vtable) != g_LegacySurfaceVTables.end())
        return;

    LegacySurfaceVTableRecord record;
    const VTableHook::Status flipStatus =
        VTableHook::Create(&vtable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurfaceLegacyFlip, (LPVOID*)&record.flip);
    const VTableHook::Status bltStatus =
        VTableHook::Create(&vtable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurfaceLegacyBlt, (LPVOID*)&record.blt);
    const VTableHook::Status lockStatus =
        VTableHook::Create(&vtable[DDSURFACE7_VTABLE_LOCK], (LPVOID)&DetourDDSurfaceLegacyLock, (LPVOID*)&record.lock);
    const VTableHook::Status unlockStatus = VTableHook::Create(
        &vtable[DDSURFACE7_VTABLE_UNLOCK], (LPVOID)&DetourDDSurfaceLegacyUnlock, (LPVOID*)&record.unlock);
    g_LegacySurfaceVTables.emplace(vtable, record);
    if (flipStatus == VTableHook::Success && bltStatus == VTableHook::Success && lockStatus == VTableHook::Success &&
        unlockStatus == VTableHook::Success && record.flip && record.blt && record.lock && record.unlock) {
        HookLog("DDraw: Legacy surface hooks installed via %s (surface=%p, vtable=%p)", reason, surface, vtable);
    } else {
        HookLogImportant("DDraw: Legacy surface hook installation incomplete via %s (flip=%s blt=%s lock=%s unlock=%s)",
                         reason, VTableHook::StatusToString(flipStatus), VTableHook::StatusToString(bltStatus),
                         VTableHook::StatusToString(lockStatus), VTableHook::StatusToString(unlockStatus));
    }
}

static void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* reason) {
    if (!ddraw)
        return;
    void** vtable = *(void***)ddraw;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    if (g_LegacyDDrawVTables.find(vtable) != g_LegacyDDrawVTables.end())
        return;

    DDrawLegacyCreateSurface_t original = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(&vtable[6], (LPVOID)&DetourDirectDrawLegacyCreateSurface, (LPVOID*)&original);
    if (status == VTableHook::Success && original) {
        g_LegacyDDrawVTables.emplace(vtable, LegacyDDrawVTableRecord{original, version});
        HookLog("DDraw: %s CreateSurface identity hook installed via %s (object=%p, vtable=%p)",
                ce::graphics_api_identity::DirectDrawLabel(version), reason, ddraw, vtable);
    } else {
        HookLogImportant("DDraw: %s CreateSurface identity hook failed via %s (%s)",
                         ce::graphics_api_identity::DirectDrawLabel(version), reason,
                         VTableHook::StatusToString(status));
    }
}

static void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* reason, bool markPrototype) {
    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - null vtable (surface=%p)", reason, surface);
        return;
    }

    if (HasHookedVTable(g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                reason, surface, surfaceVTable);
        return;
    }

    g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (markPrototype && !g_HookSurfacePrototype4)
        g_HookSurfacePrototype4 = surface;

    HookLog("DDraw: Installing surface4 hooks via %s (surface=%p, vtable=%p, prototype=%d)", reason, surface,
            surfaceVTable, markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurface4Flip,
                           oDDSurface4Flip ? nullptr : (LPVOID*)&oDDSurface4Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Flip4 hook install via %s returned %s", reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurface4Blt,
                           oDDSurface4Blt ? nullptr : (LPVOID*)&oDDSurface4Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Blt4 hook install via %s returned %s", reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_LOCK], (LPVOID)&DetourDDSurface4Lock,
                           oDDSurface4Lock ? nullptr : (LPVOID*)&oDDSurface4Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Lock4 hook install via %s returned %s", reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK], (LPVOID)&DetourDDSurface4Unlock,
                           oDDSurface4Unlock ? nullptr : (LPVOID*)&oDDSurface4Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: Unlock4 hook install via %s returned %s", reason, VTableHook::StatusToString(unlockStatus));
    }
}

static void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* reason, bool markPrototype) {
    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - null vtable (surface=%p)", reason, surface);
        return;
    }

    if (HasHookedVTable(g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                reason, surface, surfaceVTable);
        return;
    }

    g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (markPrototype && !g_HookSurfacePrototype)
        g_HookSurfacePrototype = surface;

    HookLog("DDraw: Installing surface hooks via %s (surface=%p, vtable=%p, prototype=%d)", reason, surface,
            surfaceVTable, markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurface7Flip,
                           oDDSurface7Flip ? nullptr : (LPVOID*)&oDDSurface7Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip hook installed via %s", reason);
    } else {
        HookLog("DDraw: Flip hook install via %s returned %s", reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurface7Blt,
                           oDDSurface7Blt ? nullptr : (LPVOID*)&oDDSurface7Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt hook installed via %s", reason);
    } else {
        HookLog("DDraw: Blt hook install via %s returned %s", reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_LOCK], (LPVOID)&DetourDDSurface7Lock,
                           oDDSurface7Lock ? nullptr : (LPVOID*)&oDDSurface7Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock hook installed via %s", reason);
    } else {
        HookLog("DDraw: Lock hook install via %s returned %s", reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK], (LPVOID)&DetourDDSurface7Unlock,
                           oDDSurface7Unlock ? nullptr : (LPVOID*)&oDDSurface7Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock hook installed via %s", reason);
    } else {
        HookLog("DDraw: Unlock hook install via %s returned %s", reason, VTableHook::StatusToString(unlockStatus));
    }

    IDirectDrawSurface4* surface4 = nullptr;
    if (SUCCEEDED(surface->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&surface4))) && surface4) {
        InstallSurfaceHooksForSurface4(surface4, reason, markPrototype);
        surface4->Release();
    }
}

static void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* reason) {
    if (!ddraw4) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null object", reason);
        return;
    }

    void** ddraw4VTable = *(void***)ddraw4;
    if (!ddraw4VTable) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null vtable (object=%p)", reason, ddraw4);
        return;
    }

    if (HasHookedVTable(g_HookedDDrawVTables, ddraw4VTable)) {
        HookLog(
            "DDraw: InstallDirectDraw4HooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            reason, ddraw4, ddraw4VTable);
        return;
    }

    g_HookedDDrawVTables.push_back(ddraw4VTable);
    HookLog("DDraw: Installing DirectDraw4 hooks via %s (object=%p, vtable=%p)", reason, ddraw4, ddraw4VTable);

    InstallD3D3FactoryIdentityHook(ddraw4, reason);

    DDraw4CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(&ddraw4VTable[6], (LPVOID)&DetourDirectDraw4CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        g_DDraw4CreateSurfaceOriginals.emplace(ddraw4VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: CreateSurface4 hook install via %s returned %s", reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }
}

static void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* reason) {
    if (!directDrawObject)
        return;

    IUnknown* d3d3 = nullptr;
    if (FAILED(directDrawObject->QueryInterface(kIID_IDirect3D3, reinterpret_cast<void**>(&d3d3))) || !d3d3)
        return;

    void** vtable = *(void***)d3d3;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        if (g_D3D3CreateDeviceOriginals.find(vtable) == g_D3D3CreateDeviceOriginals.end()) {
            D3D3CreateDevice_t original = nullptr;
            const VTableHook::Status status =
                VTableHook::Create(&vtable[8], (LPVOID)&DetourD3D3CreateDevice, (LPVOID*)&original);
            if (status == VTableHook::Success && original) {
                g_D3D3CreateDeviceOriginals.emplace(vtable, original);
                HookLog("DDraw: D3D6 CreateDevice identity hook installed via %s", reason);
            } else {
                HookLogImportant("DDraw: D3D6 CreateDevice identity hook failed via %s (%s)", reason,
                                 VTableHook::StatusToString(status));
            }
        }
    }
    d3d3->Release();
}

static void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* reason) {
    if (!ddraw7)
        return;

    IDirect3D7* d3d7 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(kIID_IDirect3D7, reinterpret_cast<void**>(&d3d7))) && d3d7) {
        void** vtable = *(void***)d3d7;
        {
            std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
            if (g_D3D7CreateDeviceOriginals.find(vtable) == g_D3D7CreateDeviceOriginals.end()) {
                D3D7CreateDevice_t original = nullptr;
                const VTableHook::Status status =
                    VTableHook::Create(&vtable[4], (LPVOID)&DetourD3D7CreateDevice, (LPVOID*)&original);
                if (status == VTableHook::Success && original) {
                    g_D3D7CreateDeviceOriginals.emplace(vtable, original);
                    HookLog("DDraw: D3D7 CreateDevice identity hook installed via %s", reason);
                } else {
                    HookLogImportant("DDraw: D3D7 CreateDevice identity hook failed via %s (%s)", reason,
                                     VTableHook::StatusToString(status));
                }
            }
        }
        d3d7->Release();
    }

    InstallD3D3FactoryIdentityHook(ddraw7, reason);
}

static void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* reason) {
    if (!ddraw7) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null object", reason);
        return;
    }

    void** ddraw7VTable = *(void***)ddraw7;
    if (!ddraw7VTable) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null vtable (object=%p)", reason, ddraw7);
        return;
    }

    if (HasHookedVTable(g_HookedDDrawVTables, ddraw7VTable)) {
        HookLog(
            "DDraw: InstallDirectDrawHooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            reason, ddraw7, ddraw7VTable);
        return;
    }

    g_HookedDDrawVTables.push_back(ddraw7VTable);
    HookLog("DDraw: Installing DirectDraw hooks via %s (object=%p, vtable=%p)", reason, ddraw7, ddraw7VTable);

    IDirectDraw* ddraw1 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(&ddraw1))) && ddraw1) {
        InstallLegacyDirectDrawHooksForInstance(ddraw1, ce::graphics_api_identity::DirectDrawVersion::DirectDraw,
                                                reason);
        ddraw1->Release();
    }
    IDirectDraw2* ddraw2 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&ddraw2))) && ddraw2) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw2),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw2, reason);
        ddraw2->Release();
    }
    IDirectDraw3* ddraw3 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(kIID_IDirectDraw3, reinterpret_cast<void**>(&ddraw3))) && ddraw3) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw3),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw3, reason);
        ddraw3->Release();
    }

    IDirectDraw4* ddraw4 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&ddraw4))) && ddraw4) {
        InstallDirectDraw4HooksForInstance(ddraw4, reason);
        ddraw4->Release();
    }

    InstallLegacyD3DFactoryIdentityHooks(ddraw7, reason);

    DDraw7CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(&ddraw7VTable[6], (LPVOID)&DetourDirectDraw7CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        g_DDraw7CreateSurfaceOriginals.emplace(ddraw7VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface hook installed via %s", reason);
    } else {
        HookLog("DDraw: CreateSurface hook install via %s returned %s", reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }
}

static ce::graphics_api_identity::DirectDrawVersion DirectDrawVersionFromIID(REFIID iid) {
    if (IsEqualIID(iid, IID_IDirectDraw7))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw7;
    if (IsEqualIID(iid, IID_IDirectDraw4))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw4;
    if (IsEqualIID(iid, kIID_IDirectDraw3))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw3;
    if (IsEqualIID(iid, IID_IDirectDraw2))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw2;
    if (IsEqualIID(iid, IID_IDirectDraw))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw;
    return ce::graphics_api_identity::DirectDrawVersion::Unknown;
}

bool HookDirectDrawObject(void* directDrawObject, REFIID iid) {
    HookLog("DDraw: HookDirectDrawObject called (object=%p, iidIsDDraw7=%d, iidIsDDraw4=%d)", directDrawObject,
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0);

    if (!directDrawObject)
        return false;

    if (ShouldSuppressDirectDrawHooking()) {
        return false;
    }

    const auto requestedVersion = DirectDrawVersionFromIID(iid);
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && g_DDrawBootstrapDepth != 0) {
        static std::atomic<int> s_ignoredBootstrapIdentityLogs{0};
        if (s_ignoredBootstrapIdentityLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLog("[GraphicsAPI] ignored synthetic DirectDraw bootstrap interface api=%s",
                    ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && g_DDrawBootstrapDepth == 0) {
        g_LegacyD3DCallbackVersion.store(0, std::memory_order_release);
        g_ActiveLegacyD3DVersion.store(0, std::memory_order_release);
        const int previous =
            g_ActiveDirectDrawVersion.exchange(static_cast<int>(requestedVersion), std::memory_order_acq_rel);
        if (previous != static_cast<int>(requestedVersion)) {
            HookLogImportant("[GraphicsAPI] DirectDraw interface accepted api=%s evidence=application creation",
                             ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }

    if (IsEqualIID(iid, IID_IDirectDraw7)) {
        InstallDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw7*>(directDrawObject), "wrapper CreateEx");
        return true;
    }

    if (IsEqualIID(iid, IID_IDirectDraw4)) {
        auto* ddraw4 = reinterpret_cast<IDirectDraw4*>(directDrawObject);
        InstallDirectDraw4HooksForInstance(ddraw4, "wrapper CreateEx");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(ddraw4->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper CreateEx upgrade");
            ddraw7->Release();
        }
        return true;
    }

    if (requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw2 ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw3) {
        auto* legacy = reinterpret_cast<IDirectDraw*>(directDrawObject);
        InstallLegacyDirectDrawHooksForInstance(legacy, requestedVersion, "wrapper creation");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(legacy->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper creation upgrade");
            ddraw7->Release();
        }
        return true;
    }

    return false;
}

// Common capture logic called after Flip/Blt
static void HandleCapture(IDirectDrawSurface7* primarySurface, IDirectDrawSurface7* explicitSourceSurface = nullptr) {
    g_CaptureRecurse++;
    if (g_CaptureRecurse > 1) {
        g_CaptureRecurse--;
        return;
    }

    g_RenderWatchdog.Heartbeat();

    // Update performance metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    HWND targetHwnd = ResolveDirectDrawTargetWindow();
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    const bool haveSurfaceSize =
        GetSurfaceSize(primarySurface, surfaceWidth, surfaceHeight) && surfaceWidth > 0 && surfaceHeight > 0;
    IDirectDrawSurface7* presentationSurface =
        ResolvePreferredPresentationSurface(primarySurface, explicitSourceSurface);

    static bool loggedFirstHandleCapture = false;
    if (!loggedFirstHandleCapture) {
        HookLogImportant(
            "DDraw: First HandleCapture surface=%p hwnd=%p recording=%d showOverlay=%d captureIncludeOverlay=%d "
            "size=%ux%u",
            primarySurface, targetHwnd, isRecording ? 1 : 0, shouldDrawOverlay ? 1 : 0, captureIncludeOverlay ? 1 : 0,
            surfaceWidth, surfaceHeight);
        loggedFirstHandleCapture = true;
    }

    if (shouldDrawOverlay && haveSurfaceSize) {
        g_DDrawCapture.EnsureOverlayDevice(targetHwnd, surfaceWidth, surfaceHeight);
    }

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!g_DDrawCapture.initialized && haveSurfaceSize) {
                g_DDrawCapture.EnsureCaptureResources(primarySurface, targetHwnd, surfaceWidth, surfaceHeight);
            }

            if (g_DDrawCapture.initialized) {
                g_DDrawCapture.CaptureFrameFromSurface(presentationSurface ? presentationSurface : primarySurface);
            }
        }
    };

    // Lambda for overlay drawing
    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            DrawDDrawOverlay(presentationSurface ? presentationSurface : primarySurface);
        }
    };

    // Order capture/overlay based on config
    if (captureIncludeOverlay) {
        doOverlay();  // Draw overlay first
        doCapture();  // Then capture (includes overlay)
    } else {
        doCapture();  // Capture first (clean frame)
        doOverlay();  // Then draw overlay (visible but not recorded)
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    g_CaptureRecurse--;
}

static void HandleCaptureSurface4(IDirectDrawSurface4* primarySurface,
                                  IDirectDrawSurface4* explicitSourceSurface = nullptr) {
    IDirectDrawSurface7* primarySurface7 = QuerySurface7(primarySurface);
    if (!primarySurface7) {
        static int primaryUpgradeFailLogCount = 0;
        if (primaryUpgradeFailLogCount < 4) {
            HookLog("DDraw: Failed to upgrade DirectDraw4 primary surface to DirectDraw7 for capture/overlay");
            primaryUpgradeFailLogCount++;
        }
        return;
    }

    IDirectDrawSurface7* explicitSourceSurface7 = QuerySurface7(explicitSourceSurface);
    HandleCapture(primarySurface7, explicitSourceSurface7);

    if (explicitSourceSurface7) {
        explicitSourceSurface7->Release();
    }
    primarySurface7->Release();
}

static void HandleCaptureLegacySurface(IDirectDrawSurface* primarySurface,
                                       IDirectDrawSurface* explicitSourceSurface = nullptr) {
    IDirectDrawSurface7* primarySurface7 = QuerySurface7(primarySurface);
    if (!primarySurface7) {
        static std::atomic<int> s_upgradeFailureLogCount{0};
        if (s_upgradeFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Failed to upgrade legacy primary surface to Surface7 for capture/overlay");
        }
        return;
    }
    IDirectDrawSurface7* explicitSourceSurface7 = QuerySurface7(explicitSourceSurface);
    HandleCapture(primarySurface7, explicitSourceSurface7);
    if (explicitSourceSurface7)
        explicitSourceSurface7->Release();
    primarySurface7->Release();
}

static HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* pUnkOuter) {
    LegacyDDrawVTableRecord record;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_LegacyDDrawVTables.find(pThis ? *(void***)pThis : nullptr);
        if (it != g_LegacyDDrawVTables.end())
            record = it->second;
    }
    if (!record.createSurface)
        return DDERR_GENERIC;

    if (pDesc && g_IPC) {
        const int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc) && (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX)) {
            pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
            pDesc->dwBackBufferCount = static_cast<DWORD>(count - 1);
        }
    }

    const HRESULT hr = record.createSurface(pThis, pDesc, ppSurface, pUnkOuter);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, record.version);
        InstallSurfaceHooksForLegacySurface(*ppSurface, ce::graphics_api_identity::DirectDrawLabel(record.version));
        if (g_DDrawBootstrapDepth == 0) {
            HookLog("DDraw: %s CreateSurface accepted surface=%p primary=%d",
                    ce::graphics_api_identity::DirectDrawLabel(record.version), *ppSurface,
                    IsPrimarySurfaceDesc(pDesc) ? 1 : 0);
        }
    }
    return hr;
}

static LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface) {
    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    const auto it = g_LegacySurfaceVTables.find(surface ? *(void***)surface : nullptr);
    return it != g_LegacySurfaceVTables.end() ? it->second : LegacySurfaceVTableRecord{};
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD flags) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.flip)
        return DDERR_GENERIC;
    const HRESULT hr = record.flip(surface, destOverride, flags);
    if (SUCCEEDED(hr) && g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD flags,
                                                          DDBLTFX* bltFx) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.blt)
        return DDERR_GENERIC;
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, flags, bltFx);
    if (SUCCEEDED(hr) && g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface, srcSurface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD flags, HANDLE event) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    return record.lock ? record.lock(surface, destRect, surfaceDesc, flags, event) : DDERR_GENERIC;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID surfaceData) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.unlock)
        return DDERR_GENERIC;
    const HRESULT hr = record.unlock(surface, surfaceData);
    if (SUCCEEDED(hr) && g_DDrawBootstrapDepth == 0 && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;
}

// Hook: IDirectDraw7::CreateSurface
static HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* pUnkOuter) {
    HookLog("DDraw: DetourDirectDraw7CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x)", pThis,
            pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0);

    if (pDesc && g_IPC) {
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc)) {
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                pDesc->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface: Overriding BackBufferCount to %d", count);
            }
        }
    }

    DDraw7CreateSurface_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_DDraw7CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != g_DDraw7CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw7CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        InstallSurfaceHooksForSurface(*ppSurface, "CreateSurface");
        if (IsPrimarySurfaceDesc(pDesc)) {
            g_PrimarySurface = *ppSurface;
            HookLog("DDraw: Tracking primary surface from CreateSurface (%p)", *ppSurface);
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                InstallAttachedBackBufferHooks(*ppSurface, "CreateSurface attached backbuffer");
            }
        }
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* pUnkOuter) {
    HookLog("DDraw: DetourDirectDraw4CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x)", pThis,
            pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0);

    if (pDesc && g_IPC) {
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc)) {
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                pDesc->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface4: Overriding BackBufferCount to %d", count);
            }
        }
    }

    DDraw4CreateSurface_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_DDraw4CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != g_DDraw4CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        InstallSurfaceHooksForSurface4(*ppSurface, "CreateSurface4");
        if (IsPrimarySurfaceDesc(pDesc)) {
            g_PrimarySurface4 = *ppSurface;
            HookLog("DDraw: Tracking primary surface4 from CreateSurface (%p)", *ppSurface);
        }
    }

    return hr;
}

// Hook: IDirectDrawSurface7::Flip

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD flags) {
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
    MaybeTrackPrimarySurface(surface, "Flip");

    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") {
                // Force Immediate
                flags |= 0x00000008;   // DDFLIP_NOVSYNC
                flags &= ~0x00000001;  // DDFLIP_WAIT
            } else if (mode == "fifo" || mode == "adaptive") {
                // Force Wait
                flags |= 0x00000001;   // DDFLIP_WAIT
                flags &= ~0x00000008;  // DDFLIP_NOVSYNC
            }
        }
    }

    // CPU Prerender Limit (Buffered)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0.0f) {
        ApplyPrerenderLimitDDraw(surface, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = oDDSurface7Flip(surface, destOverride, flags);

    // CPU Prerender Limit (Serial)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit == 0.0f) {
        ApplyPrerenderLimitDDraw(surface, 0.0f);
    }

    // Capture after flip (primary surface now has the rendered frame)
    HandleCapture(surface);

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD flags) {
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
    MaybeTrackPrimarySurface4(surface, "Flip4");

    HRESULT hr = oDDSurface4Flip(surface, destOverride, flags);
    HandleCaptureSurface4(surface);
    return hr;
}

// Hook: IDirectDrawSurface7::Blt
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx) {
    HRESULT hr = oDDSurface7Blt(surface, destRect, srcSurface, srcRect, flags, bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (SUCCEEDED(hr) && srcSurface && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        RememberPresentedSourceSurface(srcSurface);
    }

    if (surface != g_HookSurfacePrototype && !g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Blt");
    }

    // Only capture if this is a blit to the tracked primary surface
    if (surface && surface != g_HookSurfacePrototype && (!g_PrimarySurface || surface == g_PrimarySurface)) {
        HandleCapture(surface, srcSurface);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx) {
    HRESULT hr = oDDSurface4Blt(surface, destRect, srcSurface, srcRect, flags, bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);

    if (surface != g_HookSurfacePrototype4 && !g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Blt4");
    }

    if (SUCCEEDED(hr) && surface && surface != g_HookSurfacePrototype4 &&
        (!g_PrimarySurface4 || surface == g_PrimarySurface4)) {
        HandleCaptureSurface4(surface, srcSurface);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD flags, HANDLE event) {
    HRESULT hr = oDDSurface7Lock(surface, destRect, surfaceDesc, flags, event);
    if (SUCCEEDED(hr) && surface != g_HookSurfacePrototype && !g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Lock");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD flags, HANDLE event) {
    HRESULT hr = oDDSurface4Lock(surface, destRect, surfaceDesc, flags, event);
    if (SUCCEEDED(hr) && surface != g_HookSurfacePrototype4 && !g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Lock4");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT rect) {
    HRESULT hr = oDDSurface7Unlock(surface, rect);
    if (SUCCEEDED(hr) && surface && surface == g_PrimarySurface) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        HandleCapture(surface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT rect) {
    HRESULT hr = oDDSurface4Unlock(surface, rect);
    if (SUCCEEDED(hr) && surface && surface == g_PrimarySurface4) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        HandleCaptureSurface4(surface);
    }
    return hr;
}

static void ReportLegacyD3DUse(unsigned version, const char* evidence) {
    if (g_DDrawBootstrapDepth != 0)
        return;
    const unsigned previous = g_LegacyD3DCallbackVersion.exchange(version, std::memory_order_acq_rel);
    g_ActiveLegacyD3DVersion.store(version, std::memory_order_release);
    if (previous != version) {
        HookLogImportant("[GraphicsAPI] legacy Direct3D use accepted api=DX%u evidence=%s", version,
                         evidence ? evidence : "unknown");
    }
}

static HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** device) {
    D3D7CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_D3D7CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != g_D3D7CreateDeviceOriginals.end())
            original = it->second;
    }
    const HRESULT hr = original ? original(d3d, deviceClass, target, device) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && device && *device && g_DDrawBootstrapDepth == 0) {
        g_D3D7Device = *device;
        AssociateLegacyD3DSurface(target, 7);
        ReportLegacyD3DUse(7, "IDirect3D7::CreateDevice");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** device,
                                                        IUnknown* outer) {
    D3D3CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_D3D3CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != g_D3D3CreateDeviceOriginals.end())
            original = it->second;
    }
    const HRESULT hr = original ? original(d3d, deviceClass, target, device, outer) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && device && *device && g_DDrawBootstrapDepth == 0) {
        AssociateLegacyD3DSurface(target, 6);
        ReportLegacyD3DUse(6, "IDirect3D3::CreateDevice");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* device, DWORD Type, DWORD Value) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetRenderState");
    if (g_IPC) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (Type == 2 /* D3DRENDERSTATE_ANTIALIAS */) {
                if (strcmp(msaa, "off") == 0)
                    Value = 0;  // D3DANTIALIAS_NONE
                else
                    Value = 2;  // D3DANTIALIAS_SORTINDEPENDENT
            }
        }
    }
    return oSetRenderState7(device, Type, Value);
}

static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD Value) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetTextureStageState");
    g_D3D7Device = device;  // Capture device for proactive use
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, device, Stage, Type, Value,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState7),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState7),
        QueryD3D7MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD* pValue) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::GetTextureStageState");
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, device, Stage, Type, pValue,
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState7),
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState7),
        QueryD3D7MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* device, DWORD Stage, DWORD Type, DWORD Value) {
    ReportLegacyD3DUse(6, "IDirect3DDevice3::SetTextureStageState");
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, device, Stage, Type, Value,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState6),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState6),
        QueryD3D6MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* device, DWORD Stage, DWORD Type, DWORD* pValue) {
    ReportLegacyD3DUse(6, "IDirect3DDevice3::GetTextureStageState");
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, device, Stage, Type, pValue,
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState6),
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState6),
        QueryD3D6MaxAnisotropy);
}

static HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* pUnkOuter) {
    const HRESULT hr = oDirectDrawCreate ? oDirectDrawCreate(lpGuid, lplpDD, pUnkOuter) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && lplpDD && *lplpDD)
        HookDirectDrawObject(*lplpDD, IID_IDirectDraw);
    return hr;
}

static HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter) {
    HookLog("DDraw: DetourDirectDrawCreateEx called (iidIsDDraw7=%d, iidIsDDraw4=%d, out=%p)",
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0, lplpDD);
    HRESULT hr = oDirectDrawCreateEx ? oDirectDrawCreateEx(lpGuid, lplpDD, iid, pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDrawCreateEx returned hr=0x%08x, object=%p", hr,
            (lplpDD && SUCCEEDED(hr)) ? *lplpDD : nullptr);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
        HookDirectDrawObject(*lplpDD, iid);
    }
    return hr;
}

void DDrawHook::Init() {
    HookLog("DDrawHook::Init()");

    if (ShouldSuppressDirectDrawHooking()) {
        HookLog("DDraw: Init suppressed because DXVK d3d9 Vulkan path is active");
        return;
    }

    // Skip DDraw hooks when a higher-level D3D API (d3d9, d3d8) is already loaded.
    // ddraw.dll is often loaded as a transitive system dependency even in DX9+ games,
    // and bootstrapping DDraw hooks (which internally creates a D3D9 device via
    // DirectDrawCreateEx -> Windows DDraw-on-D3D9 mapping) can crash when third-party
    // overlays (Steam, Discord, etc.) have already hooked Direct3DCreate9 and their
    // internal state is not prepared for a synthetic device creation on a worker thread.
    //
    // BioShockInfinite crash family (2026-04-30):
    //   gameoverlayrenderer!OverlayHookD3D3+0x8ba7: FF 50 50 (call [eax+0x50])
    //   Access violation reading vtable slot at 0x6284d010 from EAX=0x6284CFC0
    //   Triggered by DDraw bootstrap calling DirectDrawCreateEx on the hook thread
    //   while Steam overlay controls the D3D9 vtable.
    if (GetModuleHandleA("d3d9.dll") || GetModuleHandleA("d3d8.dll")) {
        HookLog("DDraw: Skipping DDraw hooks (higher-level D3D API present; d3d9=%d d3d8=%d)",
                GetModuleHandleA("d3d9.dll") ? 1 : 0, GetModuleHandleA("d3d8.dll") ? 1 : 0);
        return;
    }

    // Check if ddraw.dll is loaded
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        return;
    }

    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found");
        return;
    }

    DirectDrawCreate_t pDirectDrawCreate = (DirectDrawCreate_t)GetProcAddress(ddrawModule, "DirectDrawCreate");
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);
    InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);

    if (!QueueDirectDrawBootstrapOnWindowThread()) {
        HookLog("DDraw: Falling back to hook-thread bootstrap");
        if (!g_HooksInitialized) {
            BootstrapDirectDrawHooksOnCurrentThread("hook-thread bootstrap");
        }
    } else if (!g_HooksInitialized) {
        HookLog("DDraw: Awaiting queued window-thread bootstrap callback");
    }
}

void DDrawHook::Shutdown() {
    HookLog("DDrawHook::Shutdown()");
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D6);
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D7);

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    g_DDrawCapture.CleanupDDraw(true);
}

void DDrawHook::OnHostDisconnect() {
    HookLog("DDrawHook::OnHostDisconnect()");
    g_DDrawCapture.CleanupDDraw(true);
}
