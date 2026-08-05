#pragma once

struct IDirect3D7;

struct IDirect3DDevice7;

struct LegacyDDrawVTableRecord;

struct LegacySurfaceVTableRecord;

struct LegacyD3DSamplerVTableRecord;

class DirectDrawBootstrapScope;

struct DDrawCapture;

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

// D3D7 function typedef
typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState7_t)(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                            DWORD ddraw_hook_Value);

typedef HRESULT(STDMETHODCALLTYPE* GetTextureStageState7_t)(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                            DWORD* ddraw_hook_pValue);

typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState6_t)(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value);

typedef HRESULT(STDMETHODCALLTYPE* GetTextureStageState6_t)(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue);

typedef HRESULT(STDMETHODCALLTYPE* LegacyD3DEndScene_t)(void* ddraw_hook_device);

typedef HRESULT(STDMETHODCALLTYPE* D3D7ApplyStateBlock_t)(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

typedef HRESULT(STDMETHODCALLTYPE* SetRenderState7_t)(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value);

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
                                                     DWORD ddraw_hook_flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Flip_t)(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                     DWORD ddraw_hook_flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Blt_t)(IDirectDrawSurface7* surface, LPRECT destRect,
                                                    IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                    void* ddraw_hook_bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Blt_t)(IDirectDrawSurface4* surface, LPRECT destRect,
                                                    IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                    void* ddraw_hook_bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Unlock_t)(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Unlock_t)(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Lock_t)(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Lock_t)(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7GetDC_t)(IDirectDrawSurface7* surface, HDC* hdc);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7ReleaseDC_t)(IDirectDrawSurface7* surface, HDC hdc);

typedef HRESULT(STDMETHODCALLTYPE* DDraw7CreateSurface_t)(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                          IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDraw4CreateSurface_t)(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                          IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDrawLegacyCreateSurface_t)(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                               IDirectDrawSurface** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyFlip_t)(IDirectDrawSurface* surface, IDirectDrawSurface* destOverride,
                                                          DWORD ddraw_hook_flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyBlt_t)(IDirectDrawSurface* surface, LPRECT destRect,
                                                         IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                         DDBLTFX* ddraw_hook_bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyLock_t)(IDirectDrawSurface* surface, LPRECT destRect,
                                                          DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyUnlock_t)(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData);

typedef HRESULT(STDMETHODCALLTYPE* D3D7CreateDevice_t)(IDirect3D7*, REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**);

typedef HRESULT(STDMETHODCALLTYPE* D3D3CreateDevice_t)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**,
                                                       IUnknown*);

typedef HRESULT(WINAPI* DirectDrawCreate_t)(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(WINAPI* DirectDrawCreateEx_t)(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter);

bool HookDirectDrawObject(void* directDrawObject, REFIID iid);

struct IDirect3D7 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumDevices(void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**) = 0;
};

struct IDirect3DDevice7 : public IUnknown {};

inline const GUID ddraw_hook_kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

inline const GUID ddraw_hook_kIID_IDirect3D3 = {0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e}};

inline const GUID ddraw_hook_kIID_IDirectDraw3 = {0x618f8ad4, 0x8b7a, 0x11d0, {0x8f, 0xcc, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};

inline const GUID ddraw_hook_kIID_IDirect3DHALDevice = {
    0x84E63dE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};

// Original function pointers
inline DDSurface7Flip_t ddraw_hook_oDDSurface7Flip = nullptr;

inline DDSurface4Flip_t ddraw_hook_oDDSurface4Flip = nullptr;

inline DDSurface7Blt_t ddraw_hook_oDDSurface7Blt = nullptr;

inline DDSurface4Blt_t ddraw_hook_oDDSurface4Blt = nullptr;

inline DDSurface7Lock_t ddraw_hook_oDDSurface7Lock = nullptr;

inline DDSurface4Lock_t ddraw_hook_oDDSurface4Lock = nullptr;

inline DDSurface7Unlock_t ddraw_hook_oDDSurface7Unlock = nullptr;

inline DDSurface4Unlock_t ddraw_hook_oDDSurface4Unlock = nullptr;

inline SetTextureStageState7_t ddraw_hook_oSetTextureStageState7 = nullptr;

inline GetTextureStageState7_t ddraw_hook_oGetTextureStageState7 = nullptr;

inline SetTextureStageState6_t ddraw_hook_oSetTextureStageState6 = nullptr;

inline GetTextureStageState6_t ddraw_hook_oGetTextureStageState6 = nullptr;

inline SetRenderState7_t ddraw_hook_oSetRenderState7 = nullptr;

inline DirectDrawCreate_t ddraw_hook_oDirectDrawCreate = nullptr;

inline DirectDrawCreateEx_t ddraw_hook_oDirectDrawCreateEx = nullptr;

// Globals
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline PerformanceMetrics ddraw_hook_g_PerfMetrics;

inline HWND ddraw_hook_g_CachedHwnd = NULL;

inline bool ddraw_hook_g_HooksInitialized = false;

inline IDirectDrawSurface7* ddraw_hook_g_PrimarySurface = nullptr;

inline IDirectDrawSurface4* ddraw_hook_g_PrimarySurface4 = nullptr;

inline IDirectDrawSurface7* ddraw_hook_g_HookSurfacePrototype = nullptr;

inline IDirectDrawSurface4* ddraw_hook_g_HookSurfacePrototype4 = nullptr;

inline int ddraw_hook_g_CaptureRecurse = 0;

inline std::vector<IDirectDrawSurface7*> ddraw_hook_g_PrerenderSurfaces;

inline std::vector<void**> ddraw_hook_g_HookedDDrawVTables;

inline std::vector<void**> ddraw_hook_g_HookedSurfaceVTables;

inline uint32_t ddraw_hook_g_PrerenderIdx = 0;

inline IDirect3DDevice7* ddraw_hook_g_D3D7Device = nullptr;

inline IDirectDrawSurface7* ddraw_hook_g_LastPresentedSourceSurface = nullptr;

inline DWORD ddraw_hook_g_LastPresentedSourceTick = 0;

inline bool ddraw_hook_g_DirectDrawCreateExInlineInstalled = false;

inline bool ddraw_hook_g_DirectDrawCreateInlineInstalled = false;

inline HWND ddraw_hook_g_DDrawBootstrapWindow = NULL;

inline thread_local unsigned ddraw_hook_g_DDrawBootstrapDepth = 0;

inline std::atomic<int> ddraw_hook_g_ActiveDirectDrawVersion{0};

inline std::atomic<unsigned> ddraw_hook_g_ActiveLegacyD3DVersion{0};

inline std::atomic<unsigned> ddraw_hook_g_LegacyD3DCallbackVersion{0};

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

inline std::mutex ddraw_hook_g_DDrawIdentityMutex;

inline std::unordered_map<void**, LegacyDDrawVTableRecord> ddraw_hook_g_LegacyDDrawVTables;

inline std::unordered_map<void**, LegacySurfaceVTableRecord> ddraw_hook_g_LegacySurfaceVTables;

inline std::unordered_map<void**, DDraw4CreateSurface_t> ddraw_hook_g_DDraw4CreateSurfaceOriginals;

inline std::unordered_map<void**, DDraw7CreateSurface_t> ddraw_hook_g_DDraw7CreateSurfaceOriginals;

inline std::unordered_map<void**, D3D7CreateDevice_t> ddraw_hook_g_D3D7CreateDeviceOriginals;

inline std::unordered_map<void**, D3D3CreateDevice_t> ddraw_hook_g_D3D3CreateDeviceOriginals;

inline std::unordered_map<uintptr_t, ce::graphics_api_identity::DirectDrawVersion> ddraw_hook_g_SurfaceDirectDrawVersions;

inline std::unordered_map<uintptr_t, unsigned> ddraw_hook_g_SurfaceLegacyD3DVersions;

struct LegacyD3DSamplerVTableRecord {
    ce::legacy_d3d_sampler_state::Api api = ce::legacy_d3d_sampler_state::Api::D3D6;
    void** vtable = nullptr;
    std::atomic<ce::legacy_d3d_sampler_state::SetTextureStageStateFn> setState{nullptr};
    std::atomic<ce::legacy_d3d_sampler_state::GetTextureStageStateFn> getState{nullptr};
    std::atomic<LegacyD3DEndScene_t> endScene{nullptr};
    std::atomic<D3D7ApplyStateBlock_t> applyStateBlock{nullptr};
};

inline std::mutex ddraw_hook_g_LegacyD3DSamplerVTableMutex;

inline std::vector<std::unique_ptr<LegacyD3DSamplerVTableRecord>> ddraw_hook_g_LegacyD3DSamplerVTables;

class DirectDrawBootstrapScope {
public:
    DirectDrawBootstrapScope() {
        ++ddraw_hook_g_DDrawBootstrapDepth;
    }
    ~DirectDrawBootstrapScope() {
        --ddraw_hook_g_DDrawBootstrapDepth;
    }
};

inline uintptr_t DirectDrawObjectIdentity(IUnknown* object) {
    if (!object)
        return 0;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity)
        return reinterpret_cast<uintptr_t>(object);
    const uintptr_t value = reinterpret_cast<uintptr_t>(identity);
    identity->Release();
    return value;
}

inline void AssociateDirectDrawSurface(IUnknown* surface, ce::graphics_api_identity::DirectDrawVersion version) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ddraw_hook_g_SurfaceDirectDrawVersions[identity] = version;
}

inline void AssociateLegacyD3DSurface(IUnknown* surface, unsigned d3dVersion) {
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ddraw_hook_g_SurfaceLegacyD3DVersions[identity] = d3dVersion;
}

inline void ActivateDirectDrawSurface(IUnknown* surface, ce::graphics_api_identity::DirectDrawVersion fallbackVersion) {
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

inline UINT QueryD3D7MaxAnisotropy(void* opaqueDevice) {
    if (!opaqueDevice)
        return 1;
    auto* ddraw_hook_device = static_cast<IDirect3DDevice7*>(opaqueDevice);
    void** vtable = *(void***)ddraw_hook_device;
    using GetCaps7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, D3DDEVICEDESC7*);
    auto getCaps = reinterpret_cast<GetCaps7_t>(vtable[3]);
    D3DDEVICEDESC7 caps = {};
    return getCaps && SUCCEEDED(getCaps(ddraw_hook_device, &caps)) ? std::max<DWORD>(1, caps.dwMaxAnisotropy) : 1;
}

inline UINT QueryD3D6MaxAnisotropy(void* opaqueDevice) {
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

inline bool ShouldSuppressDirectDrawHooking() {
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

inline bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& ddraw_hook_h);

inline bool HasHookedVTable(const std::vector<void**>& hookedVTables, void** vtable) {
    return std::find(hookedVTables.begin(), hookedVTables.end(), vtable) != hookedVTables.end();
}

inline bool IsPrimarySurfaceDesc(const DDSURFACEDESC2* surfaceDesc) {
    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
}

inline bool IsPrimarySurfaceDesc(const DDSURFACEDESC* surfaceDesc) {
    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
}

inline bool SurfaceHasCaps(IDirectDrawSurface* surface, DWORD capsMask) {
    if (!surface)
        return false;
    DDSCAPS caps = {};
    return SUCCEEDED(surface->GetCaps(&caps)) && (caps.dwCaps & capsMask) != 0;
}

inline bool SurfaceHasCaps(IDirectDrawSurface7* surface, DWORD capsMask) {
    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;
}

inline bool SurfaceHasCaps(IDirectDrawSurface4* surface, DWORD capsMask) {
    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;
}

inline IDirectDrawSurface7* QuerySurface7(IUnknown* surfaceLike) {
    if (!surfaceLike)
        return nullptr;

    IDirectDrawSurface7* surface7 = nullptr;
    if (FAILED(surfaceLike->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&surface7)))) {
        return nullptr;
    }

    return surface7;
}

inline void RememberPresentedSourceSurface(IDirectDrawSurface7* surface) {
    if (!surface)
        return;

    ddraw_hook_g_LastPresentedSourceSurface = surface;
    ddraw_hook_g_LastPresentedSourceTick = GetTickCount();
}

inline IDirectDrawSurface7* ResolvePreferredPresentationSurface(IDirectDrawSurface7* primarySurface,
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
    if (ddraw_hook_g_LastPresentedSourceSurface && (now - ddraw_hook_g_LastPresentedSourceTick) <= 100 &&
        surfaceMatchesPrimary(ddraw_hook_g_LastPresentedSourceSurface)) {
        return ddraw_hook_g_LastPresentedSourceSurface;
    }

    return primarySurface;
}

inline HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

inline HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

inline HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* ddraw_hook_pUnkOuter);

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD ddraw_hook_flags);

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                          DDBLTFX* ddraw_hook_bltFx);

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD ddraw_hook_flags);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD ddraw_hook_flags);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect);

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect);

inline HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value);

inline HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD ddraw_hook_Value);

inline HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD* ddraw_hook_pValue);

inline HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value);

inline HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue);

inline HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* ddraw_hook_device);

inline HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

inline HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* ddraw_hook_device);

inline HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** ddraw_hook_device);

inline HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** ddraw_hook_device,
                                                        IUnknown* ddraw_hook_outer);

inline HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter);

inline HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter);

inline void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* ddraw_hook_reason);

inline void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason, bool ddraw_hook_markPrototype = false);

inline void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason,
                                           bool ddraw_hook_markPrototype = false);

inline void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* ddraw_hook_reason);

inline void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* ddraw_hook_reason);

inline void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* ddraw_hook_reason);

inline void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* ddraw_hook_reason);

inline void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* ddraw_hook_reason);

inline void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t ddraw_hook_directDrawCreate);

inline void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t ddraw_hook_directDrawCreateEx);

inline void BootstrapDirectDrawHooksOnCurrentThread(const char* ddraw_hook_reason);

inline LegacyD3DSamplerVTableRecord* ResolveLegacyD3DSamplerVTable(
    ce::legacy_d3d_sampler_state::Api api, void* ddraw_hook_device) {
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

inline void InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api api, void* ddraw_hook_device, bool newDevice,
                                        const char* ddraw_hook_reason) {
    if (!ddraw_hook_device)
        return;

    void** vtable = *(void***)ddraw_hook_device;
    LegacyD3DSamplerVTableRecord* record = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_LegacyD3DSamplerVTableMutex);
        for (const auto& candidate : ddraw_hook_g_LegacyD3DSamplerVTables) {
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
            ddraw_hook_g_LegacyD3DSamplerVTables.push_back(std::move(newRecord));
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
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[setSlot]), setDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->setState.store(original, std::memory_order_release);
                if (isD3D7 && !ddraw_hook_oSetTextureStageState7)
                    ddraw_hook_oSetTextureStageState7 = reinterpret_cast<SetTextureStageState7_t>(original);
                if (!isD3D7 && !ddraw_hook_oSetTextureStageState6)
                    ddraw_hook_oSetTextureStageState6 = reinterpret_cast<SetTextureStageState6_t>(original);
            }
        }
        if (!record->getState.load(std::memory_order_acquire)) {
            ce::legacy_d3d_sampler_state::GetTextureStageStateFn original = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[getSlot]), getDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->getState.store(original, std::memory_order_release);
                if (isD3D7 && !ddraw_hook_oGetTextureStageState7)
                    ddraw_hook_oGetTextureStageState7 = reinterpret_cast<GetTextureStageState7_t>(original);
                if (!isD3D7 && !ddraw_hook_oGetTextureStageState6)
                    ddraw_hook_oGetTextureStageState6 = reinterpret_cast<GetTextureStageState6_t>(original);
            }
        }
        if (!record->endScene.load(std::memory_order_acquire)) {
            LegacyD3DEndScene_t original = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[endSceneSlot]), endSceneDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->endScene.store(original, std::memory_order_release);
            }
        }
        if (isD3D7 && !record->applyStateBlock.load(std::memory_order_acquire)) {
            D3D7ApplyStateBlock_t original = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D7_VTABLE_APPLYSTATEBLOCK]),
                                   reinterpret_cast<LPVOID>(&DetourD3D7ApplyStateBlock),
                                   reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
                record->applyStateBlock.store(original, std::memory_order_release);
            }
        }
    }

    auto queryMaxAnisotropy = api == ce::legacy_d3d_sampler_state::Api::D3D7 ? QueryD3D7MaxAnisotropy
                                                                              : QueryD3D6MaxAnisotropy;
    ce::legacy_d3d_sampler_state::RegisterDevice(api, ddraw_hook_device, newDevice, queryMaxAnisotropy);
    HookLog("DDraw: DX%u sampler hooks reconciled vtable=%p reason=%s", api == ce::legacy_d3d_sampler_state::Api::D3D7 ? 7u : 6u,
            vtable, ddraw_hook_reason ? ddraw_hook_reason : "unknown");
}

inline HWND ResolveDirectDrawTargetWindow() {
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

inline void MaybeTrackPrimarySurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason) {
    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype || ddraw_hook_g_PrimarySurface)
        return;

    ddraw_hook_g_PrimarySurface = surface;
    HookLog("DDraw: Tracking runtime primary surface from %s (%p)", ddraw_hook_reason, surface);
}

inline void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason) {
    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype4 || ddraw_hook_g_PrimarySurface4)
        return;

    ddraw_hook_g_PrimarySurface4 = surface;
    HookLog("DDraw: Tracking runtime primary surface4 from %s (%p)", ddraw_hook_reason, surface);
}

inline void ApplyPrerenderLimitDDraw(IDirectDrawSurface7* surface, float limit) {
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

inline void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t ddraw_hook_directDrawCreate) {
    if (!ddraw_hook_directDrawCreate || ddraw_hook_g_DirectDrawCreateInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)ddraw_hook_directDrawCreate, (void*)DetourDirectDrawCreate, &trampoline)) {
        ddraw_hook_oDirectDrawCreate = reinterpret_cast<DirectDrawCreate_t>(trampoline);
        ddraw_hook_g_DirectDrawCreateInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreate inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreate inline hook failed");
    }
}

inline void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t ddraw_hook_directDrawCreateEx) {
    if (!ddraw_hook_directDrawCreateEx || ddraw_hook_g_DirectDrawCreateExInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)ddraw_hook_directDrawCreateEx, (void*)DetourDirectDrawCreateEx, &trampoline)) {
        ddraw_hook_oDirectDrawCreateEx = reinterpret_cast<DirectDrawCreateEx_t>(trampoline);
        ddraw_hook_g_DirectDrawCreateExInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreateEx inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreateEx inline hook failed");
    }
}

inline void BootstrapDirectDrawHooksOnCurrentThread(const char* ddraw_hook_reason) {
    if (ddraw_hook_g_HooksInitialized)
        return;
    DirectDrawBootstrapScope bootstrapScope;

    HookLog("DDraw: BootstrapDirectDrawHooksOnCurrentThread starting via %s", ddraw_hook_reason);

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

    DirectDrawCreateEx_t createFunction = ddraw_hook_oDirectDrawCreateEx ? ddraw_hook_oDirectDrawCreateEx : pDirectDrawCreateEx;
    HookLog("DDraw: Bootstrap create function=%p (export=%p, trampoline=%p)", createFunction, pDirectDrawCreateEx,
            ddraw_hook_oDirectDrawCreateEx);

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

    InstallDirectDrawHooksForInstance(ddraw7, ddraw_hook_reason);

    DDSURFACEDESC2 ddsd = {};
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    IDirectDrawSurface7* dummySurface = nullptr;
    hr = ddraw7->CreateSurface(&ddsd, &dummySurface, NULL);
    HookLog("DDraw: Bootstrap CreateSurface returned hr=0x%08x, surface=%p", hr, dummySurface);

    if (SUCCEEDED(hr) && dummySurface) {
        InstallSurfaceHooksForSurface(dummySurface, ddraw_hook_reason, true);

        IDirect3D7* d3d7 = nullptr;
        if (SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirect3D7, (void**)&d3d7))) {
            IDirect3DDevice7* d3d7Device = nullptr;
            if (SUCCEEDED(d3d7->CreateDevice(ddraw_hook_kIID_IDirect3DHALDevice, dummySurface, &d3d7Device))) {
                void** d3d7DeviceVTable = *(void***)d3d7Device;
                InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D7, d3d7Device, false,
                                            "bootstrap");

                if (VTableHook::Create(reinterpret_cast<void*>(&d3d7DeviceVTable[D3D7_VTABLE_SETRENDERSTATE]), (LPVOID)&DetourSetRenderState7,
                                       (LPVOID*)&ddraw_hook_oSetRenderState7) == VTableHook::Success) {
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
            SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirect3D3, (void**)&d3d3))) {
            using CreateDevice3_t =
                HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**, IUnknown*);
            void** d3d3VTable = *(void***)d3d3;
            auto createDevice3 = reinterpret_cast<CreateDevice3_t>(d3d3VTable[8]);
            IUnknown* d3d6Device = nullptr;
            if (createDevice3 &&
                SUCCEEDED(createDevice3(d3d3, ddraw_hook_kIID_IDirect3DHALDevice, dummySurface4, &d3d6Device, nullptr))) {
                InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D6, d3d6Device, false,
                                            "bootstrap");
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

    ddraw_hook_g_HooksInitialized = true;
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

    void CreateSharedResources(uint32_t w, uint32_t ddraw_hook_h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
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
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
            d3d9 = ce::security::LoadSystemLibrary(L"d3d9.dll");
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
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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

    bool EnsureOverlayDevice(HWND hwnd, uint32_t w, uint32_t ddraw_hook_h) {
        if (!hwnd || w == 0 || ddraw_hook_h == 0) {
            static int invalidOverlayStateLogCount = 0;
            if (invalidOverlayStateLogCount < 3) {
                HookLog("DDraw: EnsureOverlayDevice skipped (hwnd=%p, size=%ux%u)", hwnd, w, ddraw_hook_h);
                invalidOverlayStateLogCount++;
            }
            return false;
        }

        const bool hwndChanged = targetHwnd && hwnd != targetHwnd;
        const bool sizeChanged = width != w || height != ddraw_hook_h;
        if (initialized && (hwndChanged || sizeChanged)) {
            // Capture owns the generation-wide dimensions. Let
            // EnsureCaptureResources drain/rebuild it before mutating them.
            return false;
        }
        if ((hwndChanged || sizeChanged) && d3d9DeviceEx) {
            HookLog("DDraw: Recreating overlay helper (oldHwnd=%p newHwnd=%p old=%ux%u new=%ux%u)", targetHwnd, hwnd,
                    width, height, w, ddraw_hook_h);
            ReleaseOverlayResources();
        }

        targetHwnd = hwnd;
        width = w;
        height = ddraw_hook_h;
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

    bool EnsureCaptureResources(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t ddraw_hook_h) {
        if (!surface || w == 0 || ddraw_hook_h == 0) {
            HookLog("DDraw: EnsureCaptureResources skipped (surface=%p, size=%ux%u)", surface, w, ddraw_hook_h);
            return false;
        }

        if (initialized && ddrawSurface == surface && width == w && height == ddraw_hook_h) {
            if (hwnd) {
                targetHwnd = hwnd;
            }
            return true;
        }

        if (initialized) {
            HookLog(
                "DDraw: Reinitializing capture resources for new surface/size (oldSurface=%p newSurface=%p old=%ux%u "
                "new=%ux%u)",
                ddrawSurface, surface, width, height, w, ddraw_hook_h);
            if (!CleanupDDraw(false))
                return false;
        }

        ddrawSurface = surface;
        targetHwnd = hwnd;
        width = w;
        height = ddraw_hook_h;
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

        EnsureOverlayDevice(hwnd, w, ddraw_hook_h);

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

    void Init(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t ddraw_hook_h) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        EnsureCaptureResources(surface, hwnd, w, ddraw_hook_h);
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
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -(int)height;  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ oldBm = SelectObject(memDC, hbm);

        // BitBlt from surface DC
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        BitBlt(memDC, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

        // Capture the bits
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        CaptureFrame(bits, width * 4);

        // Cleanup
        SelectObject(memDC, oldBm);
        DeleteObject(hbm);
        DeleteDC(memDC);

        surface->ReleaseDC(hdc);
    }
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline DDrawCapture ddraw_hook_g_DDrawCapture;

// Draw overlay using D3D9Ex
inline void DrawDDrawOverlay(IDirectDrawSurface7* overlaySourceSurface) {
    if (!ddraw_hook_g_DDrawCapture.d3d9DeviceEx)
        return;

    if (ddraw_hook_g_DDrawCapture.targetHwnd && ddraw_hook_g_DDrawCapture.targetHwnd != ddraw_hook_g_CachedHwnd) {
        ddraw_hook_g_CachedHwnd = ddraw_hook_g_DDrawCapture.targetHwnd;
        InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
    }

    if (ddraw_hook_g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        ddraw_hook_g_CachedHwnd = ddraw_hook_g_DDrawCapture.targetHwnd;
        if (ddraw_hook_g_CachedHwnd) {
            InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
            g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
        }
        if (g_OverlayAdapter.InitDX9(ddraw_hook_g_DDrawCapture.d3d9DeviceEx)) {
            if (ddraw_hook_g_CachedHwnd) {
                g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
            }
            HookLog("DDraw: OverlayAdapter initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&ddraw_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(ddraw_hook_g_DDrawCapture.droppedFrames.load(std::memory_order_relaxed));
    const auto directDrawVersion = static_cast<ce::graphics_api_identity::DirectDrawVersion>(
        ddraw_hook_g_ActiveDirectDrawVersion.load(std::memory_order_acquire));
    const unsigned d3dVersion = ddraw_hook_g_ActiveLegacyD3DVersion.load(std::memory_order_acquire);
    g_OverlayAdapter.SetGraphicsAPI(ce::graphics_api_identity::LegacyDirectXLabel(directDrawVersion, d3dVersion),
                                    "active DirectDraw presentation surface");

    if (g_OverlayAdapter.IsInitialized() && ddraw_hook_g_DDrawCapture.width > 0 && ddraw_hook_g_DDrawCapture.height > 0) {
        ddraw_hook_g_DDrawCapture.CopyPrimarySurfaceToOverlayBackbuffer(overlaySourceSurface);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        g_OverlayAdapter.RenderOverlay(ddraw_hook_g_DDrawCapture.width, ddraw_hook_g_DDrawCapture.height);
        static uint32_t overlayRenderSubmitCount = 0;
        overlayRenderSubmitCount++;
        if (overlayRenderSubmitCount <= 8) {
            HookLogImportant("DDraw: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)",
                             ddraw_hook_g_DDrawCapture.targetHwnd, ddraw_hook_g_DDrawCapture.width, ddraw_hook_g_DDrawCapture.height,
                             overlayRenderSubmitCount);
        }
        ddraw_hook_g_DDrawCapture.PresentOverlay();
    }
}

inline void InstallAttachedBackBufferHooks(IDirectDrawSurface7* primarySurface, const char* ddraw_hook_reason) {
    if (!primarySurface) {
        return;
    }

    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer) {
        InstallSurfaceHooksForSurface(backBuffer, ddraw_hook_reason);
        backBuffer->Release();
    }
}

// Get surface dimensions from DDSURFACEDESC2
inline bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& ddraw_hook_h) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);

    if (surface && SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
        w = desc.dwWidth;
        ddraw_hook_h = desc.dwHeight;
        return true;
    }
    return false;
}

inline void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* ddraw_hook_reason) {
    if (!surface)
        return;
    void** vtable = *(void***)surface;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (ddraw_hook_g_LegacySurfaceVTables.find(vtable) != ddraw_hook_g_LegacySurfaceVTables.end())
        return;

    LegacySurfaceVTableRecord record;
    const VTableHook::Status flipStatus =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_FLIP]), (LPVOID)&DetourDDSurfaceLegacyFlip, (LPVOID*)&record.flip);
    const VTableHook::Status bltStatus =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_BLT]), (LPVOID)&DetourDDSurfaceLegacyBlt, (LPVOID*)&record.blt);
    const VTableHook::Status lockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_LOCK]), (LPVOID)&DetourDDSurfaceLegacyLock, (LPVOID*)&record.lock);
    const VTableHook::Status unlockStatus = VTableHook::Create(
        reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_UNLOCK]), (LPVOID)&DetourDDSurfaceLegacyUnlock, (LPVOID*)&record.unlock);
    ddraw_hook_g_LegacySurfaceVTables.emplace(vtable, record);
    if (flipStatus == VTableHook::Success && bltStatus == VTableHook::Success && lockStatus == VTableHook::Success &&
        unlockStatus == VTableHook::Success && record.flip && record.blt && record.lock && record.unlock) {
        HookLog("DDraw: Legacy surface hooks installed via %s (surface=%p, vtable=%p)", ddraw_hook_reason, surface, vtable);
    } else {
        HookLogImportant("DDraw: Legacy surface hook installation incomplete via %s (flip=%s blt=%s lock=%s unlock=%s)",
                         ddraw_hook_reason, VTableHook::StatusToString(flipStatus), VTableHook::StatusToString(bltStatus),
                         VTableHook::StatusToString(lockStatus), VTableHook::StatusToString(unlockStatus));
    }
}

inline void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* ddraw_hook_reason) {
    if (!ddraw)
        return;
    void** vtable = *(void***)ddraw;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (ddraw_hook_g_LegacyDDrawVTables.find(vtable) != ddraw_hook_g_LegacyDDrawVTables.end())
        return;

    DDrawLegacyCreateSurface_t original = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[6]), (LPVOID)&DetourDirectDrawLegacyCreateSurface, (LPVOID*)&original);
    if (status == VTableHook::Success && original) {
        ddraw_hook_g_LegacyDDrawVTables.emplace(vtable, LegacyDDrawVTableRecord{original, version});
        HookLog("DDraw: %s CreateSurface identity hook installed via %s (object=%p, vtable=%p)",
                ce::graphics_api_identity::DirectDrawLabel(version), ddraw_hook_reason, ddraw, vtable);
    } else {
        HookLogImportant("DDraw: %s CreateSurface identity hook failed via %s (%s)",
                         ce::graphics_api_identity::DirectDrawLabel(version), ddraw_hook_reason,
                         VTableHook::StatusToString(status));
    }
}

inline void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason, bool ddraw_hook_markPrototype) {
    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - null vtable (surface=%p)", ddraw_hook_reason, surface);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                ddraw_hook_reason, surface, surfaceVTable);
        return;
    }

    ddraw_hook_g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (ddraw_hook_markPrototype && !ddraw_hook_g_HookSurfacePrototype4)
        ddraw_hook_g_HookSurfacePrototype4 = surface;

    HookLog("DDraw: Installing surface4 hooks via %s (surface=%p, vtable=%p, prototype=%d)", ddraw_hook_reason, surface,
            surfaceVTable, ddraw_hook_markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_FLIP]), (LPVOID)&DetourDDSurface4Flip,
                           ddraw_hook_oDDSurface4Flip ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Flip4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_BLT]), (LPVOID)&DetourDDSurface4Blt,
                           ddraw_hook_oDDSurface4Blt ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Blt4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_LOCK]), (LPVOID)&DetourDDSurface4Lock,
                           ddraw_hook_oDDSurface4Lock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Lock4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK]), (LPVOID)&DetourDDSurface4Unlock,
                           ddraw_hook_oDDSurface4Unlock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Unlock4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(unlockStatus));
    }
}

inline void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason, bool ddraw_hook_markPrototype) {
    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - null vtable (surface=%p)", ddraw_hook_reason, surface);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                ddraw_hook_reason, surface, surfaceVTable);
        return;
    }

    ddraw_hook_g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (ddraw_hook_markPrototype && !ddraw_hook_g_HookSurfacePrototype)
        ddraw_hook_g_HookSurfacePrototype = surface;

    HookLog("DDraw: Installing surface hooks via %s (surface=%p, vtable=%p, prototype=%d)", ddraw_hook_reason, surface,
            surfaceVTable, ddraw_hook_markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_FLIP]), (LPVOID)&DetourDDSurface7Flip,
                           ddraw_hook_oDDSurface7Flip ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Flip hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_BLT]), (LPVOID)&DetourDDSurface7Blt,
                           ddraw_hook_oDDSurface7Blt ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Blt hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_LOCK]), (LPVOID)&DetourDDSurface7Lock,
                           ddraw_hook_oDDSurface7Lock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Lock hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK]), (LPVOID)&DetourDDSurface7Unlock,
                           ddraw_hook_oDDSurface7Unlock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Unlock hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(unlockStatus));
    }

    IDirectDrawSurface4* surface4 = nullptr;
    if (SUCCEEDED(surface->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&surface4))) && surface4) {
        InstallSurfaceHooksForSurface4(surface4, ddraw_hook_reason, ddraw_hook_markPrototype);
        surface4->Release();
    }
}

inline void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* ddraw_hook_reason) {
    if (!ddraw4) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null object", ddraw_hook_reason);
        return;
    }

    void** ddraw4VTable = *(void***)ddraw4;
    if (!ddraw4VTable) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null vtable (object=%p)", ddraw_hook_reason, ddraw4);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedDDrawVTables, ddraw4VTable)) {
        HookLog(
            "DDraw: InstallDirectDraw4HooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            ddraw_hook_reason, ddraw4, ddraw4VTable);
        return;

    }

    ddraw_hook_g_HookedDDrawVTables.push_back(ddraw4VTable);
    HookLog("DDraw: Installing DirectDraw4 hooks via %s (object=%p, vtable=%p)", ddraw_hook_reason, ddraw4, ddraw4VTable);

    InstallD3D3FactoryIdentityHook(ddraw4, ddraw_hook_reason);

    DDraw4CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(ddraw_hook_g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(reinterpret_cast<void*>(&ddraw4VTable[6]), (LPVOID)&DetourDirectDraw4CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        ddraw_hook_g_DDraw4CreateSurfaceOriginals.emplace(ddraw4VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: CreateSurface4 hook install via %s returned %s", ddraw_hook_reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }
}

inline void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* ddraw_hook_reason) {
    if (!directDrawObject)
        return;

    IUnknown* d3d3 = nullptr;
    if (FAILED(directDrawObject->QueryInterface(ddraw_hook_kIID_IDirect3D3, reinterpret_cast<void**>(&d3d3))) || !d3d3)
        return;

    void** vtable = *(void***)d3d3;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        if (ddraw_hook_g_D3D3CreateDeviceOriginals.find(vtable) == ddraw_hook_g_D3D3CreateDeviceOriginals.end()) {
            D3D3CreateDevice_t original = nullptr;
            const VTableHook::Status status =
                VTableHook::Create(reinterpret_cast<void*>(&vtable[8]), (LPVOID)&DetourD3D3CreateDevice, (LPVOID*)&original);
            if (status == VTableHook::Success && original) {
                ddraw_hook_g_D3D3CreateDeviceOriginals.emplace(vtable, original);
                HookLog("DDraw: D3D6 CreateDevice identity hook installed via %s", ddraw_hook_reason);
            } else {
                HookLogImportant("DDraw: D3D6 CreateDevice identity hook failed via %s (%s)", ddraw_hook_reason,
                                 VTableHook::StatusToString(status));
            }
        }
    }
    d3d3->Release();
}

inline void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* ddraw_hook_reason) {
    if (!ddraw7)
        return;

    IDirect3D7* d3d7 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirect3D7, reinterpret_cast<void**>(&d3d7))) && d3d7) {
        void** vtable = *(void***)d3d7;
        {
            std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
            if (ddraw_hook_g_D3D7CreateDeviceOriginals.find(vtable) == ddraw_hook_g_D3D7CreateDeviceOriginals.end()) {
                D3D7CreateDevice_t original = nullptr;
                const VTableHook::Status status =
                    VTableHook::Create(reinterpret_cast<void*>(&vtable[4]), (LPVOID)&DetourD3D7CreateDevice, (LPVOID*)&original);
                if (status == VTableHook::Success && original) {
                    ddraw_hook_g_D3D7CreateDeviceOriginals.emplace(vtable, original);
                    HookLog("DDraw: D3D7 CreateDevice identity hook installed via %s", ddraw_hook_reason);
                } else {
                    HookLogImportant("DDraw: D3D7 CreateDevice identity hook failed via %s (%s)", ddraw_hook_reason,
                                     VTableHook::StatusToString(status));
                }
            }
        }
        d3d7->Release();
    }

    InstallD3D3FactoryIdentityHook(ddraw7, ddraw_hook_reason);
}

inline void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* ddraw_hook_reason) {
    if (!ddraw7) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null object", ddraw_hook_reason);
        return;
    }

    void** ddraw7VTable = *(void***)ddraw7;
    if (!ddraw7VTable) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null vtable (object=%p)", ddraw_hook_reason, ddraw7);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedDDrawVTables, ddraw7VTable)) {
        HookLog(
            "DDraw: InstallDirectDrawHooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            ddraw_hook_reason, ddraw7, ddraw7VTable);
        return;
    }

    ddraw_hook_g_HookedDDrawVTables.push_back(ddraw7VTable);
    HookLog("DDraw: Installing DirectDraw hooks via %s (object=%p, vtable=%p)", ddraw_hook_reason, ddraw7, ddraw7VTable);

    IDirectDraw* ddraw1 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(&ddraw1))) && ddraw1) {
        InstallLegacyDirectDrawHooksForInstance(ddraw1, ce::graphics_api_identity::DirectDrawVersion::DirectDraw,
                                                ddraw_hook_reason);
        ddraw1->Release();
    }
    IDirectDraw2* ddraw2 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&ddraw2))) && ddraw2) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw2),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw2, ddraw_hook_reason);
        ddraw2->Release();
    }
    IDirectDraw3* ddraw3 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirectDraw3, reinterpret_cast<void**>(&ddraw3))) && ddraw3) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw3),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw3, ddraw_hook_reason);
        ddraw3->Release();
    }

    IDirectDraw4* ddraw4 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&ddraw4))) && ddraw4) {
        InstallDirectDraw4HooksForInstance(ddraw4, ddraw_hook_reason);
        ddraw4->Release();
    }

    InstallLegacyD3DFactoryIdentityHooks(ddraw7, ddraw_hook_reason);

    DDraw7CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(ddraw_hook_g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(reinterpret_cast<void*>(&ddraw7VTable[6]), (LPVOID)&DetourDirectDraw7CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        ddraw_hook_g_DDraw7CreateSurfaceOriginals.emplace(ddraw7VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: CreateSurface hook install via %s returned %s", ddraw_hook_reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }
}

// Common capture logic called after Flip/Blt
inline void HandleCapture(IDirectDrawSurface7* primarySurface, IDirectDrawSurface7* explicitSourceSurface = nullptr) {
    ddraw_hook_g_CaptureRecurse++;
    if (ddraw_hook_g_CaptureRecurse > 1) {
        ddraw_hook_g_CaptureRecurse--;
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
    ddraw_hook_g_PerfMetrics.Update(us);

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
        ddraw_hook_g_DDrawCapture.EnsureOverlayDevice(targetHwnd, surfaceWidth, surfaceHeight);
    }

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!ddraw_hook_g_DDrawCapture.initialized && haveSurfaceSize) {
                ddraw_hook_g_DDrawCapture.EnsureCaptureResources(primarySurface, targetHwnd, surfaceWidth, surfaceHeight);
            }

            if (ddraw_hook_g_DDrawCapture.initialized) {
                ddraw_hook_g_DDrawCapture.CaptureFrameFromSurface(presentationSurface ? presentationSurface : primarySurface);
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

    ddraw_hook_g_CaptureRecurse--;
}

inline void HandleCaptureSurface4(IDirectDrawSurface4* primarySurface,
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

inline void HandleCaptureLegacySurface(IDirectDrawSurface* primarySurface,
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

inline HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* ddraw_hook_pUnkOuter) {
    LegacyDDrawVTableRecord record;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_LegacyDDrawVTables.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_LegacyDDrawVTables.end())
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

    const HRESULT hr = record.createSurface(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, record.version);
        InstallSurfaceHooksForLegacySurface(*ppSurface, ce::graphics_api_identity::DirectDrawLabel(record.version));
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            HookLog("DDraw: %s CreateSurface accepted surface=%p primary=%d",
                    ce::graphics_api_identity::DirectDrawLabel(record.version), *ppSurface,
                    IsPrimarySurfaceDesc(pDesc) ? 1 : 0);
        }
    }
    return hr;
}

inline LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface) {
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    const auto it = ddraw_hook_g_LegacySurfaceVTables.find(surface ? *(void***)surface : nullptr);
    return it != ddraw_hook_g_LegacySurfaceVTables.end() ? it->second : LegacySurfaceVTableRecord{};
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD ddraw_hook_flags) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.flip)
        return DDERR_GENERIC;
    const HRESULT hr = record.flip(surface, destOverride, ddraw_hook_flags);
    if (SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                          DDBLTFX* ddraw_hook_bltFx) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.blt)
        return DDERR_GENERIC;
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface, srcSurface);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags, HANDLE ddraw_hook_event) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    return record.lock ? record.lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event) : DDERR_GENERIC;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.unlock)
        return DDERR_GENERIC;
    const HRESULT hr = record.unlock(surface, ddraw_hook_surfaceData);
    if (SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;
}

// Hook: IDirectDraw7::CreateSurface
inline HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter) {
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
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_DDraw7CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_DDraw7CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw7CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        InstallSurfaceHooksForSurface(*ppSurface, "CreateSurface");
        if (IsPrimarySurfaceDesc(pDesc)) {
            ddraw_hook_g_PrimarySurface = *ppSurface;
            HookLog("DDraw: Tracking primary surface from CreateSurface (%p)", *ppSurface);
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                InstallAttachedBackBufferHooks(*ppSurface, "CreateSurface attached backbuffer");
            }
        }
    }

    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter) {
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
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_DDraw4CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_DDraw4CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        InstallSurfaceHooksForSurface4(*ppSurface, "CreateSurface4");
        if (IsPrimarySurfaceDesc(pDesc)) {
            ddraw_hook_g_PrimarySurface4 = *ppSurface;
            HookLog("DDraw: Tracking primary surface4 from CreateSurface (%p)", *ppSurface);
        }
    }

    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD ddraw_hook_flags) {
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
    MaybeTrackPrimarySurface(surface, "Flip");

    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") {
                // Force Immediate
                ddraw_hook_flags |= 0x00000008;   // DDFLIP_NOVSYNC
                ddraw_hook_flags &= ~0x00000001;  // DDFLIP_WAIT
            } else if (mode == "fifo" || mode == "adaptive") {
                // Force Wait
                ddraw_hook_flags |= 0x00000001;   // DDFLIP_WAIT
                ddraw_hook_flags &= ~0x00000008;  // DDFLIP_NOVSYNC
            }
        }
    }

    // CPU Prerender Limit (Buffered)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0.0f) {
        ApplyPrerenderLimitDDraw(surface, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = ddraw_hook_oDDSurface7Flip(surface, destOverride, ddraw_hook_flags);

    // CPU Prerender Limit (Serial)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit == 0.0f) {
        ApplyPrerenderLimitDDraw(surface, 0.0f);
    }

    // Capture after flip (primary surface now has the rendered frame)
    HandleCapture(surface);

    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD ddraw_hook_flags) {
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
    MaybeTrackPrimarySurface4(surface, "Flip4");

    HRESULT hr = ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags);
    HandleCaptureSurface4(surface);
    return hr;
}

// Hook: IDirectDrawSurface7::Blt
inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx) {
    HRESULT hr = ddraw_hook_oDDSurface7Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (SUCCEEDED(hr) && srcSurface && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        RememberPresentedSourceSurface(srcSurface);
    }

    if (surface != ddraw_hook_g_HookSurfacePrototype && !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Blt");
    }

    // Only capture if this is a blit to the tracked primary surface
    if (surface && surface != ddraw_hook_g_HookSurfacePrototype && (!ddraw_hook_g_PrimarySurface || surface == ddraw_hook_g_PrimarySurface)) {
        HandleCapture(surface, srcSurface);
    }

    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx) {
    HRESULT hr = ddraw_hook_oDDSurface4Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);

    if (surface != ddraw_hook_g_HookSurfacePrototype4 && !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Blt4");
    }

    if (SUCCEEDED(hr) && surface && surface != ddraw_hook_g_HookSurfacePrototype4 &&
        (!ddraw_hook_g_PrimarySurface4 || surface == ddraw_hook_g_PrimarySurface4)) {
        HandleCaptureSurface4(surface, srcSurface);
    }

    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event) {
    HRESULT hr = ddraw_hook_oDDSurface7Lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event);
    if (SUCCEEDED(hr) && surface != ddraw_hook_g_HookSurfacePrototype && !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Lock");
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event) {
    HRESULT hr = ddraw_hook_oDDSurface4Lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event);
    if (SUCCEEDED(hr) && surface != ddraw_hook_g_HookSurfacePrototype4 && !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Lock4");
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect) {
    HRESULT hr = ddraw_hook_oDDSurface7Unlock(surface, ddraw_hook_rect);
    if (SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        HandleCapture(surface);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect) {
    HRESULT hr = ddraw_hook_oDDSurface4Unlock(surface, ddraw_hook_rect);
    if (SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface4) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        HandleCaptureSurface4(surface);
    }
    return hr;
}

inline void ReportLegacyD3DUse(unsigned version, const char* evidence) {
    if (ddraw_hook_g_DDrawBootstrapDepth != 0)
        return;
    const unsigned previous = ddraw_hook_g_LegacyD3DCallbackVersion.exchange(version, std::memory_order_acq_rel);
    ddraw_hook_g_ActiveLegacyD3DVersion.store(version, std::memory_order_release);
    if (previous != version) {
        HookLogImportant("[GraphicsAPI] legacy Direct3D use accepted api=DX%u evidence=%s", version,
                         evidence ? evidence : "unknown");
    }
}

inline HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** ddraw_hook_device) {
    D3D7CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_D3D7CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != ddraw_hook_g_D3D7CreateDeviceOriginals.end())
            original = it->second;
    }

    const HRESULT hr = original ? original(d3d, deviceClass, target, ddraw_hook_device) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && ddraw_hook_device && *ddraw_hook_device) {
        InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D7, *ddraw_hook_device,
                                    ddraw_hook_g_DDrawBootstrapDepth == 0, "IDirect3D7::CreateDevice");
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            ddraw_hook_g_D3D7Device = *ddraw_hook_device;
            AssociateLegacyD3DSurface(target, 7);
            ReportLegacyD3DUse(7, "IDirect3D7::CreateDevice");
        }
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** ddraw_hook_device,
                                                        IUnknown* ddraw_hook_outer) {
    D3D3CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_D3D3CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != ddraw_hook_g_D3D3CreateDeviceOriginals.end())
            original = it->second;
    }
    const HRESULT hr = original ? original(d3d, deviceClass, target, ddraw_hook_device, ddraw_hook_outer) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && ddraw_hook_device && *ddraw_hook_device) {
        InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D6, *ddraw_hook_device,
                                    ddraw_hook_g_DDrawBootstrapDepth == 0, "IDirect3D3::CreateDevice");
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            AssociateLegacyD3DSurface(target, 6);
            ReportLegacyD3DUse(6, "IDirect3D3::CreateDevice");
        }
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetRenderState");
    if (g_IPC) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (Type == 2 /* D3DRENDERSTATE_ANTIALIAS */) {
                if (strcmp(msaa, "off") == 0)
                    ddraw_hook_Value = 0;  // D3DANTIALIAS_NONE
                else
                    ddraw_hook_Value = 2;  // D3DANTIALIAS_SORTINDEPENDENT
            }
        }
    }
    return ddraw_hook_oSetRenderState7(ddraw_hook_device, Type, ddraw_hook_Value);
}

inline HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD ddraw_hook_Value) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetTextureStageState");
    ddraw_hook_g_D3D7Device = ddraw_hook_device;  // Capture device for proactive use
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState7);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState7);
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, Stage, Type, ddraw_hook_Value, setState, getState, QueryD3D7MaxAnisotropy);
}

inline HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD* ddraw_hook_pValue) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::GetTextureStageState");
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState7);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState7);
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, Stage, Type, ddraw_hook_pValue, getState, setState,
        QueryD3D7MaxAnisotropy);
}

inline HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value) {
    ReportLegacyD3DUse(6, "IDirect3DDevice3::SetTextureStageState");
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState6);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState6);
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device, Stage, Type, ddraw_hook_Value, setState, getState, QueryD3D6MaxAnisotropy);
}

inline HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue) {
    ReportLegacyD3DUse(6, "IDirect3DDevice3::GetTextureStageState");
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState6);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState6);
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device, Stage, Type, ddraw_hook_pValue, getState, setState,
        QueryD3D6MaxAnisotropy);
}

inline HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* ddraw_hook_device) {
    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto endScene = record ? record->endScene.load(std::memory_order_acquire) : nullptr;
    if (!endScene)
        return DDERR_GENERIC;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    return endScene(ddraw_hook_device);
}

inline HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle) {
    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto applyStateBlock = record ? record->applyStateBlock.load(std::memory_order_acquire) : nullptr;
    if (!applyStateBlock)
        return DDERR_GENERIC;
    const HRESULT hr = applyStateBlock(ddraw_hook_device, ddraw_hook_blockHandle);
    if (SUCCEEDED(hr)) {
        ce::legacy_d3d_sampler_state::ReconcileAfterExternalStateChange(
            ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
            record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    }
    return hr;
}

inline HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* ddraw_hook_device) {
    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device);
    auto endScene = record ? record->endScene.load(std::memory_order_acquire) : nullptr;
    if (!endScene)
        return DDERR_GENERIC;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D6MaxAnisotropy);
    return endScene(ddraw_hook_device);
}

inline HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter) {
    const HRESULT hr = ddraw_hook_oDirectDrawCreate ? ddraw_hook_oDirectDrawCreate(lpGuid, lplpDD, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && lplpDD && *lplpDD)
        HookDirectDrawObject(*lplpDD, IID_IDirectDraw);
    return hr;
}

inline HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter) {
    HookLog("DDraw: DetourDirectDrawCreateEx called (iidIsDDraw7=%d, iidIsDDraw4=%d, out=%p)",
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0, lplpDD);
    HRESULT hr = ddraw_hook_oDirectDrawCreateEx ? ddraw_hook_oDirectDrawCreateEx(lpGuid, lplpDD, iid, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDrawCreateEx returned hr=0x%08x, object=%p", hr,
            (lplpDD && SUCCEEDED(hr)) ? *lplpDD : nullptr);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
        HookDirectDrawObject(*lplpDD, iid);
    }
    return hr;
}
